#define NOMINMAX
// API Abstraction
#include "PrimeEngine/APIAbstraction/APIAbstractionDefines.h"

#include "ServerNetworkManager.h"

// Outer-Engine includes

// Inter-Engine includes

#include "PrimeEngine/Lua/LuaEnvironment.h"

// additional lua includes needed
extern "C"
{
#include "PrimeEngine/../luasocket_dist/src/socket.h"
#include "PrimeEngine/../luasocket_dist/src/inet.h"
};

#include "PrimeEngine/../../GlobalConfig/GlobalConfig.h"

#include "PrimeEngine/GameObjectModel/GameObjectManager.h"
#include "PrimeEngine/Events/StandardEvents.h"
#include "PrimeEngine/Scene/DebugRenderer.h"

#include "PrimeEngine/Networking/StreamManager.h"
#include "PrimeEngine/Networking/EventManager.h"
#include "PrimeEngine/Networking/GhostManager.h"

// Sibling/Children includes
#include "ServerConnectionManager.h"

// dario note: comment this out if you want to use tcp
#define USEUDP

using namespace PE::Events;

namespace PE {

namespace Components {

PE_IMPLEMENT_CLASS1(ServerNetworkManager, NetworkManager);

ServerNetworkManager::ServerNetworkManager(PE::GameContext &context, PE::MemoryArena arena, Handle hMyself)
: NetworkManager(context, arena, hMyself)
, m_clientConnections(context, arena, PE_SERVER_MAX_CONNECTIONS)
{
	m_state = ServerState_Uninitialized;
}

ServerNetworkManager::~ServerNetworkManager()
{
	if (m_state != ServerState_Uninitialized)
		socket_destroy(&m_sock);
}

void ServerNetworkManager::addDefaultComponents()
{
	NetworkManager::addDefaultComponents();

	// no need to register handler as parent class already has this method registered
	// PE_REGISTER_EVENT_HANDLER(Events::Event_UPDATE, ServerConnectionManager::do_UPDATE);

	PE_REGISTER_EVENT_HANDLER(PE::Events::Event_CLIENT_JOIN, ServerNetworkManager::do_CLIENT_JOIN);
}

void ServerNetworkManager::initNetwork()
{
	NetworkManager::initNetwork();

	serverOpenSocket();
}

void ServerNetworkManager::serverOpenSocket()
{
	bool created = false;
	int numTries = 0;
	int port = PE_SERVER_PORT;
	
	while (!created)
	{
#ifndef USEUDP
		const char *err = /*luasocket::*/inet_trycreate(&m_sock, SOCK_STREAM);
#else
		const char *err = /*luasocket::*/inet_trycreate(&m_sock, SOCK_DGRAM);
#endif
		if (err)
		{
			assert(!"error creating socket occurred");
			break;
		}

		err = inet_trybind(&m_sock, "127.0.0.1", (unsigned short)(port)); // leaves socket non-blocking
		numTries++;
		if (err) {
			if (numTries >= 10)
				break; // give up
			port++;
		}
		else
		{
			PEINFO("PE: Server created at %s:%d\n", "127.0.0.1", port);
			created = true;
			break;
		}
	}

	if (created)
		m_serverPort = port;
	else
	{
		assert(!"Could not create server");
		return;
	}

#ifndef USEUDP
	const char *err = inet_trylisten(&m_sock, PE_SERVER_MAX_CONNECTIONS); // leaves socket non-blocking
	if (err)
	{
		PEINFO("Warning: Could not listen on socket. Err: %s\n", err);
		assert(!"Could not listen on socket");
		return;
	}
#else
	m_connectionsMutex.lock();
	createNetworkConnectionContext(m_sock, -1, &m_netContext);
	m_connectionsMutex.unlock();
#endif

	m_state = ServerState_ConnectionListening;
}

void ServerNetworkManager::createNetworkConnectionContext(t_socket sock,  int clientId, PE::NetworkContext *pNetContext)
{
	
	pNetContext->m_clientId = clientId;

	NetworkManager::createNetworkConnectionContext(sock, pNetContext);

	{
		pNetContext->m_pConnectionManager = new (m_arena) ServerConnectionManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getConnectionManager()->addDefaultComponents();
	}

	{
		pNetContext->m_pStreamManager = new (m_arena) StreamManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getStreamManager()->addDefaultComponents();
	}

	{
		pNetContext->m_pEventManager = new (m_arena) EventManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getEventManager()->addDefaultComponents();
	}

	{
		pNetContext->m_pGhostManager = new (m_arena) GhostManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getGhostManager()->addDefaultComponents();
	}

	pNetContext->getConnectionManager()->initializeConnected(sock);

	addComponent(pNetContext->getConnectionManager()->getHandle());
	addComponent(pNetContext->getStreamManager()->getHandle());
}



void ServerNetworkManager::do_UPDATE(Events::Event *pEvt)
{
	NetworkManager::do_UPDATE(pEvt);

	m_netContext.getGhostManager()->tryPrepareAll();
#ifndef USEUDP
	t_timeout timeout; // timeout supports managing timeouts of multiple blocking alls by using total.
	// but if total is < 0 it just uses block value for each blocking call
	timeout.block = 0;
	timeout.total = -1.0;
	timeout.start = 0;

	t_socket sock;
	int err = socket_accept(&m_sock, &sock, NULL, NULL, &timeout);
	if (err != IO_DONE)
	{
		const char *s = socket_strerror(err);
		return;
	}

	if (err == IO_DONE)
	{
		m_connectionsMutex.lock();
		m_clientConnections.add(NetworkContext());
		int clientIndex = m_clientConnections.m_size-1;
		NetworkContext &netContext = m_clientConnections[clientIndex];

		// create a tribes stack for this connection
		createNetworkConnectionContext(sock, clientIndex, &netContext);
		m_connectionsMutex.unlock();

		PE::Events::Event_SERVER_CLIENT_CONNECTION_ACK evt(*m_pContext);
		evt.m_clientId = clientIndex;

		netContext.getEventManager()->scheduleEvent(&evt, m_pContext->getGameObjectManager(), true);

	}
#endif
}

void ServerNetworkManager::do_CLIENT_JOIN(Events::Event *pEvt)
{
	// used only in udp
	Event_CLIENT_JOIN *pRealEvt = (Event_CLIENT_JOIN *)pEvt;

	const char *strAddr = "127.0.0.1";
	int port = PE_SERVER_PORT;
	t_socket sock;
	bool created = false;
	int numTries = 0;

	while (!created)
	{
		const char *err = /*luasocket::*/inet_trycreate(&sock, SOCK_DGRAM);

		sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;

		//dario todo: change to proper stuff
		sin.sin_port = htons(port);
		sin.sin_addr.s_addr = inet_addr(strAddr);

		err = inet_trybind(&sock, inet_ntoa(sin.sin_addr), port);
		if (err) {
			PEINFO("PE: Warning: Failed to bind to %s:%d reason: %s\n", strAddr, port, err);
			numTries++;
			if (numTries >= 10)
			{
				break; // give up
			}
			port += 13;
		}
		else
		{
			m_connectionsMutex.lock();
			m_clientConnections.add(NetworkContext());
			int clientIndex = m_clientConnections.m_size - 1;
			NetworkContext &netContext = m_clientConnections[clientIndex];
			createNetworkConnectionContext(sock, clientIndex, &netContext);
			m_connectionsMutex.unlock();

			PEINFO("PE: Created server port for client %d at %s:%d\n", clientIndex, strAddr, port);
			created = true;

			netContext.getConnectionManager()->m_targetSock = pRealEvt->m_targetSock;

			PE::Events::Event_SERVER_CLIENT_CONNECTION_ACK evt(*m_pContext);
			evt.m_clientId = clientIndex;
			evt.m_targetSock = sin;

			netContext.getEventManager()->scheduleEvent(&evt, m_pContext->getGameObjectManager(), true);
		}
	}
}

void ServerNetworkManager::debugRender(int &threadOwnershipMask, float xoffset /* = 0*/, float yoffset /* = 0*/)
{
	sprintf(PEString::s_buf, "Server: Port %d %d Connections", m_serverPort, m_clientConnections.m_size);
	DebugRenderer::Instance()->createTextMesh(
		PEString::s_buf, true, false, false, false, 0,
		Vector3(xoffset, yoffset, 0), 1.0f, threadOwnershipMask);

	float dy = 0.025f;
	float dx = 0.01;
	float evtManagerDy = 0.2f;
	// debug render all networking contexts
	m_connectionsMutex.lock();

	for (unsigned int i = 0; i < m_clientConnections.m_size; ++i)
	{
		sprintf(PEString::s_buf, "Connection[%d]:", i);
	
		DebugRenderer::Instance()->createTextMesh(
		PEString::s_buf, true, false, false, false, 0,
		Vector3(xoffset, yoffset + dy + evtManagerDy * i, 0), 1.0f, threadOwnershipMask);

		NetworkContext &netContext = m_clientConnections[i];
		netContext.getEventManager()->debugRender(threadOwnershipMask, xoffset + dx, yoffset + dy * 2.0f + evtManagerDy * i);
		netContext.getGhostManager()->debugRender(threadOwnershipMask, xoffset + dx, yoffset + dy * 2.0f + evtManagerDy * i + .1f);
	}
	m_connectionsMutex.unlock();
}

void ServerNetworkManager::scheduleEventTo(PE::Networkable *pNetworkable, PE::Networkable *pNetworkableTarget, int client)
{
	for (unsigned int i = 0; i < m_clientConnections.m_size; ++i)
	{
		if ((int)(i) != client)
			continue;

		NetworkContext &netContext = m_clientConnections[i];
		netContext.getEventManager()->scheduleEvent(pNetworkable, pNetworkableTarget, true);
		return;
	}
}

void ServerNetworkManager::scheduleEventToAll(PE::Networkable *pNetworkable, PE::Networkable *pNetworkableTarget)
{
	for (unsigned int i = 0; i < m_clientConnections.m_size; ++i)
	{
		NetworkContext &netContext = m_clientConnections[i];
		netContext.getEventManager()->scheduleEvent(pNetworkable, pNetworkableTarget, true);
	}
}

void ServerNetworkManager::scheduleEventToAllExcept(PE::Networkable *pNetworkable, PE::Networkable *pNetworkableTarget, int exceptClient)
{
	for (unsigned int i = 0; i < m_clientConnections.m_size; ++i)
	{
		if ((int)(i) == exceptClient)
			continue;

		NetworkContext &netContext = m_clientConnections[i];
		netContext.getEventManager()->scheduleEvent(pNetworkable, pNetworkableTarget, true);
	}
}



#if 0 // template
//////////////////////////////////////////////////////////////////////////
// ConnectionManager Lua Interface
//////////////////////////////////////////////////////////////////////////
//
void ConnectionManager::SetLuaFunctions(PE::Components::LuaEnvironment *pLuaEnv, lua_State *luaVM)
{
	
	//static const struct luaL_Reg l_functions[] = {
	//	{"l_clientConnectToTCPServer", l_clientConnectToTCPServer},
	//	{NULL, NULL} // sentinel
	//};

	//luaL_register(luaVM, 0, l_functions);
	
	lua_register(luaVM, "l_clientConnectToTCPServer", l_clientConnectToTCPServer);


	// run a script to add additional functionality to Lua side of Skin
	// that is accessible from Lua
// #if APIABSTRACTION_IOS
// 	LuaEnvironment::Instance()->runScriptWorkspacePath("Code/PrimeEngine/Scene/Skin.lua");
// #else
// 	LuaEnvironment::Instance()->runScriptWorkspacePath("Code\\PrimeEngine\\Scene\\Skin.lua");
// #endif

}

int ConnectionManager::l_clientConnectToTCPServer(lua_State *luaVM)
{
	lua_Number lPort = lua_tonumber(luaVM, -1);
	int port = (int)(lPort);

	const char *strAddr = lua_tostring(luaVM, -2);

	GameContext *pContext = (GameContext *)(lua_touserdata(luaVM, -3));

	lua_pop(luaVM, 3);

	pContext->getConnectionManager()->clientConnectToTCPServer(strAddr, port);

	return 0; // no return values
}

//////////////////////////////////////////////////////////////////////////
#endif
	
}; // namespace Components
}; // namespace PE
