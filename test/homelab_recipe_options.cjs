const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
let ts;
for (const relative of [
  'src/app/node_modules/typescript',
  'src/web-app/node_modules/typescript',
  'build/web-app-staging/web-app/node_modules/typescript',
  'build/web-app-manual-staging/web-app/node_modules/typescript',
  'build-local/web-app-manual-staging/web-app/node_modules/typescript',
]) {
  try {
    ts = require(path.join(root, relative));
    break;
  } catch (error) {
    if (error.code !== 'MODULE_NOT_FOUND') throw error;
  }
}
if (!ts) throw new Error('Build the web app before running the recipe options test');

const file = path.join(root, 'src/app/src/renderer/recipes/recipeOptionsConfig.ts');
const source = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;
const output = {};
vm.runInNewContext(source, { exports: output, require });

const args = '--temp 0.6 --top-p 0.95 --top-k 20 --min-p 0 --spec-type none';
const options = output.apiToRecipeOptions('hybrid', {
  ctx_size: 32768, hybrid_copy_gib: 24, llamacpp_args: args, merge_args: true,
});
const body = output.recipeOptionsToApi(options);
assert.equal(body.ctx_size, 32768);
assert.equal(body.hybrid_copy_gib, 24);
assert.equal(body.llamacpp_args, args);
assert.equal(body.merge_args, true);
assert(!('llamacpp_backend' in body), 'Hybrid must keep its fixed system launcher');
assert.equal(output.clampOptionValue('hybridCopyGib', 99), 32);
assert.equal(output.clampOptionValue('hybridCopyGib', 0), 1);

const speech = output.recipeOptionsToApi(output.apiToRecipeOptions('parakeet', {
  parakeet_threads: 4,
}));
assert.equal(speech.parakeet_threads, 4);
assert(!('ctx_size' in speech));
assert.equal(output.clampOptionValue('parakeetThreads', 99), 12);
assert.equal(output.clampOptionValue('parakeetThreads', 4.5), 5);
const tts = output.recipeOptionsToApi(output.createDefaultOptions('kokoro'));
assert('pinned' in tts && 'save_options' in tts);
assert(!('ctx_size' in tts));
console.log('Recipe options passed: saved hybrid sampling/context, fixed backend and bounded speech threads');
