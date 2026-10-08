const fs = require('node:fs');

function patch(source) {
  const before = 'existingMcpPerms?.[librechat_data_provider.Permissions.CREATE] === true && !mcpCreateExplicit';
  const after = 'existingMcpPerms?.[librechat_data_provider.Permissions.CREATE] === true && existingMcpPerms?.[librechat_data_provider.Permissions.CONFIGURE_OBO] === undefined && !mcpCreateExplicit';
  if (source.split(before).length !== 2) {
    throw new Error('LibreChat permission migration changed; review the upstream implementation before publishing');
  }
  return source.replace(before, after);
}

if (require.main === module) {
  const path = process.argv[2];
  if (!path) throw new Error('Provide the compiled API path');
  fs.writeFileSync(path, patch(fs.readFileSync(path, 'utf8')));
}
module.exports = { patch };
