// Runs the fork's own specs with the CLI's toolchain and its test setup (throwaway data dirs).
//   cd cli && npx vitest run --config ../fork/tests/vitest.config.ts
import { defineConfig } from 'vitest/config'
import { fileURLToPath } from 'node:url'

const cli = fileURLToPath(new URL('../../cli', import.meta.url))
const here = fileURLToPath(new URL('.', import.meta.url))

export default defineConfig({
  root: cli,
  server: { fs: { allow: [cli, here, fileURLToPath(new URL('../..', import.meta.url))] } },
  test: {
    environment: 'node',
    setupFiles: [`${cli}/vitest.setup.ts`],
    dir: here,
    include: ['**/*.spec.ts'],
    testTimeout: 120_000,
  },
})
