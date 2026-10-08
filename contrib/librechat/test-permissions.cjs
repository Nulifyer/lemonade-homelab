const assert = require('node:assert/strict');
const { updateInterfacePermissions } = require('/app/packages/api/dist/index.cjs');

async function checkMigratedRole(hasMigrationMarker) {
  const mcp = { USE: true, CREATE: true, SHARE: false, SHARE_PUBLIC: false };
  if (hasMigrationMarker) mcp.CONFIGURE_OBO = false;
  const updates = [];
  await updateInterfacePermissions({
    appConfig: { config: {}, interfaceConfig: {} },
    getRoleByName: async () => ({ permissions: { MCP_SERVERS: { ...mcp } } }),
    updateAccessPermissions: async (role, permissions) => updates.push({ role, permissions }),
  });
  const user = updates.find(x => x.role === 'USER');
  const create = user?.permissions.MCP_SERVERS?.CREATE;
  assert.equal(create, hasMigrationMarker ? undefined : false);
}

(async () => {
  await checkMigratedRole(false);
  await checkMigratedRole(true);
  console.log('Original USER migration runs once; subsequent UI choices are preserved');
  process.exit(0);
})().catch(error => { console.error(error); process.exit(1); });
