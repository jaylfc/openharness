/**
 * Proves the REAL client code accepts the fork's manifests: the CLI's self-updater (fetchManifest /
 * shouldAutoUpdate / downloadVerified / canary from cli/src/lib/selfUpdate.ts) and the dial firmware
 * offer (fetchRelease / shouldOffer / loadImage from cli/src/cable/fwPush.ts), fed a manifest exactly
 * as the fork publishes it.
 *
 * Two modes:
 *  - default: builds a tiny fork channel in a temp dir with make_manifests.py, serves it over loopback
 *    HTTP and runs the clients against that. Runs in the test job on every build.
 *  - FORK_MANIFEST_BASE=<https://raw.githubusercontent.com/<repo>/<branch>/harness>: runs the same
 *    assertions against a PUBLISHED channel, downloading its real artifacts. The verify job uses this
 *    against the dry-run branch.
 */
import { createServer, type Server } from 'node:http'
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { afterAll, beforeAll, describe, expect, it } from 'vitest'

import { env } from '../../cli/src/config/env.js'
import { canary, downloadVerified, fetchManifest, shouldAutoUpdate } from '../../cli/src/lib/selfUpdate.js'
import { CIRCLE_OTA_KEY, fetchRelease, loadImage, otaKeyForBoard, shouldOffer } from '../../cli/src/cable/fwPush.js'

const scripts = fileURLToPath(new URL('../scripts/', import.meta.url))
const remote = process.env.FORK_MANIFEST_BASE?.replace(/\/$/, '')
const isRemote = !!remote

let base = remote ?? ''
let server: Server | undefined
let expectFwVersion: string | undefined
const tmp = mkdtempSync(join(tmpdir(), 'fork-channel-'))

/** A minimal channel: real make_manifests.py output over stand-in artifacts, served on loopback. */
async function buildFixture(): Promise<string> {
  const assets = join(tmp, 'assets')
  const channel = join(tmp, 'channel')
  mkdirSync(assets, { recursive: true })
  // cli.js must survive the canary (`node cli.js version` exits 0)
  writeFileSync(join(assets, 'cli.js'), 'process.stdout.write("9.9.9")\n')
  writeFileSync(join(assets, 'notify.mjs'), 'export {}\n')
  // A firmware image with the esp_app_desc the device compares: magic at 0x20, version at 0x30.
  const image = Buffer.alloc(4096)
  image.writeUInt32LE(0xabcd5432, 0x20)
  image.write('9.9.9', 0x30)
  writeFileSync(join(assets, 'interns_commander.bin'), image)
  writeFileSync(join(assets, 'Harness-macos-arm64.zip'), 'zip')
  writeFileSync(join(tmp, 'plan.json'), JSON.stringify({
    desktop: { version: '9.9.9' }, cli: { version: '9.9.9' }, firmware: { version: '9.9.9' },
  }))
  server = createServer((req, res) => {
    // /assets/<name> = release downloads; anything else is the channel branch
    const path = req.url!.split('?')[0]
    try {
      const file = path.startsWith('/assets/') ? join(assets, path.slice(8)) : join(channel, path)
      res.end(readFileSync(file))
    } catch { res.statusCode = 404; res.end() }
  })
  await new Promise<void>((r) => server!.listen(0, '127.0.0.1', r))
  const port = (server.address() as { port: number }).port
  execFileSync('python3', [
    join(scripts, 'make_manifests.py'), '--assets', assets, '--plan', join(tmp, 'plan.json'),
    '--channel', channel, '--repo', 'owner/repo', '--tag', 'fixture',
  ])
  // Point the manifests' URLs at the loopback server instead of github.com.
  for (const rel of ['cli', 'esp32/ota', 'desktop']) {
    const p = join(channel, 'harness', rel, 'metadata.json')
    writeFileSync(p, readFileSync(p, 'utf8').replaceAll('https://github.com/owner/repo/releases/download/fixture', `http://127.0.0.1:${port}/assets`))
  }
  return `http://127.0.0.1:${port}/harness`
}

beforeAll(async () => { if (!isRemote) base = await buildFixture() })
afterAll(() => { server?.close() })

describe('the fork channel as the CLI defaults see it', () => {
  it('ships the fork manifests as the defaults, and leaves runtime manifests on upstream', () => {
    expect(env.ADAPTER_UPDATE_URL).toBe('https://raw.githubusercontent.com/jaylfc/openharness/fork-updates/harness/cli/metadata.json')
    expect(env.CABLE_FW_MANIFEST_URL).toBe('https://raw.githubusercontent.com/jaylfc/openharness/fork-updates/harness/esp32/ota/metadata.json')
    expect(env.ADAPTER_UPDATE_KEY).toBe('cli')
    expect(env.ADAPTER_RUNTIME_METADATA_URL).toContain('storage.googleapis.com/s3-autonomous-upgrade-3/harness/runtime/')
    expect(env.ADAPTER_GRID_RUNTIME_METADATA_URL).toContain('storage.googleapis.com/s3-autonomous-upgrade-3/harness/runtime/grid/')
  })
})

describe('CLI self-update against the fork manifest', () => {
  it('parses the manifest and offers it to an upstream build, but never over a dev build or an equal one', async () => {
    const entry = await fetchManifest(`${base}/cli/metadata.json`, env.ADAPTER_UPDATE_KEY)
    expect(entry, 'fetchManifest returned null: the manifest shape was rejected').not.toBeNull()
    expect(entry!.version).toMatch(/^\d+\.\d+\.\d+$/)
    // strictly newer than what upstream had published when this fork build was planned
    if (isRemote) {
      const upstream = await (await fetch('https://storage.googleapis.com/s3-autonomous-upgrade-3/harness/cli/metadata.json')).json() as { cli: { version: string } }
      expect(shouldAutoUpdate(entry!.version, upstream.cli.version), 'fork must outrank upstream latest').toBe(true)
    }
    expect(shouldAutoUpdate(entry!.version, '0.0.1')).toBe(true)
    expect(shouldAutoUpdate(entry!.version, entry!.version)).toBe(false)
    expect(shouldAutoUpdate(entry!.version, `${entry!.version}-dev.abc123`)).toBe(false)
    expect(shouldAutoUpdate(entry!.version, '0.0.1-dev.abc123')).toBe(false)
  })

  it('downloads both files, verifies their sha256, and the bundle runs and reports the manifest version', async () => {
    const entry = (await fetchManifest(`${base}/cli/metadata.json`, 'cli'))!
    const cli = await downloadVerified(entry.cli)
    const notify = await downloadVerified(entry.notify)
    expect(createHash('sha256').update(cli).digest('hex')).toBe(entry.cli.sha256)
    expect(notify.length).toBe(entry.notify.size)
    expect(canary(cli, tmp), 'canary (node cli.js version) failed').toBe(true)
    if (isRemote) {
      // The one check that proves manifest and bytes describe the SAME build (release.yml does the same).
      const dir = mkdtempSync(join(tmp, 'run-'))
      writeFileSync(join(dir, 'package.json'), '{"type":"module"}\n')
      writeFileSync(join(dir, 'cli.js'), cli)
      const said = execFileSync(process.execPath, [join(dir, 'cli.js'), 'version'], { encoding: 'utf8' }).trim()
      expect(said).toBe(entry.version)
      // ...and that this bundle polls the fork, not upstream
      expect(cli.toString('latin1')).toContain('fork-updates/harness/cli/metadata.json')
    }
  })

  it('rejects a tampered download', async () => {
    const entry = (await fetchManifest(`${base}/cli/metadata.json`, 'cli'))!
    await expect(downloadVerified({ ...entry.cli, sha256: '0'.repeat(64) })).rejects.toThrow(/sha256 mismatch/)
  })
})

describe('dial firmware offer against the fork manifest', () => {
  it('finds the commander entry and offers it only to older clean builds', async () => {
    const release = await fetchRelease(`${base}/esp32/ota/metadata.json`, CIRCLE_OTA_KEY)
    if (!release) {
      // A run that skipped firmware (no firmware directory in the tree) has no entry; that is allowed.
      expect(isRemote).toBe(true)
      return
    }
    expectFwVersion = release.version
    expect(otaKeyForBoard(undefined)).toBe(CIRCLE_OTA_KEY)
    expect(shouldOffer('0.0.86', release.version)).toBe(true)
    expect(shouldOffer(release.version, release.version)).toBe(false)
    expect(shouldOffer('v0.0.86-36-gbc64073-dirty', release.version)).toBe(false)
    const image = await loadImage(release, join(tmp, 'fwcache'))
    expect(image, 'loadImage rejected the image (size or sha256 mismatch)').not.toBeNull()
    // esp_app_desc: magic word at 0x20, 32-byte version string at 0x30. fw_update.c compares this
    // string to the offered version, so it must equal the manifest's exactly.
    expect(image!.readUInt32LE(0x20)).toBe(0xabcd5432)
    expect(image!.subarray(0x30, 0x50).toString('latin1').replace(/\0.*$/s, '')).toBe(release.version)
    expect(expectFwVersion).toBeDefined()
  })
})
