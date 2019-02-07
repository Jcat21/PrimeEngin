
-- this script is in lua format
-- this is a meta script that fills in data that is passed to 'myScript' that in turn calls C++ function
-- some of the data can be set by default, some of the data might be required to be edited from maya
function fillMetaInfoTable(args) -- the script fromat requires existance of this function

-- user modifed data
args['myScript']="StaticMesh.lua"
args['myScriptPackage']="Default"

args['meshName'] = "nazicar.x_carmesh_mesh.mesha"
args['meshPackage'] = "Default"

args['physicsShapes'] = {}
args['physicsShapes'][0] = {type = "box", scale = {2,1,6}, translate = {0,0,0}}

end -- required
