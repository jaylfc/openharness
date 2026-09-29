// The loopback endpoint `harness dial wifi` talks to, driven by the real command.
import type { Server } from 'node:http'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { startHookServer } from '../hookServer.js'
import { dialWifiCommand, type DialWifiDeps } from './dialWifiCommand.js'

const PSK = 'not-a-real-password-123'

describe('POST /api/dial/wifi', () => {
  let server: Server | null = null
  afterEach(async () => { if (server) await new Promise<void>(r => server!.close(() => r())); server = null })

  async function boot(onDialWifi?: NonNullable<Parameters<typeof startHookServer>[1]['onDialWifi']>) {
    const started = await startHookServer(0, { onRegistered: vi.fn(), onSessionEnd: vi.fn(), onDialWifi })
    server = started.server
    return `http://127.0.0.1:${started.port}`
  }
  const post = (base: string, body: unknown, headers: Record<string, string> = { 'x-adapter-local': '1' }) =>
    fetch(`${base}/api/dial/wifi`, { method: 'POST', headers: { 'content-type': 'application/json', ...headers }, body: JSON.stringify(body) })

  it('carries the password from the CLI to the handler in the body, and echoes nothing back', async () => {
    const seen = vi.fn(async (req: { op: string; ssid?: string; psk?: string; device?: string }) =>
      ({ status: 200, body: { ok: true, ...(req.op === 'status' ? { devices: [] } : {}) } }))
    const base = await boot(seen)
    const out: string[] = []
    const err: string[] = []
    const deps: DialWifiDeps = {
      call: async (method, path, body) => {
        const res = await fetch(`${base}${path}`, { method, headers: { 'x-adapter-local': '1', ...(body ? { 'content-type': 'application/json' } : {}) }, body: body ? JSON.stringify(body) : undefined })
        return { status: res.status, json: (await res.json()) as Record<string, unknown> }
      },
      isTTY: false, env: { HARNESS_WIFI_PSK: PSK },
      promptPassword: async () => '', output: (l: string) => out.push(l), error: (l: string) => err.push(l), sleep: async () => {}, waitMs: 1,
    }
    await dialWifiCommand(['Home'], deps)
    expect(seen).toHaveBeenCalledWith({ op: 'set', ssid: 'Home', psk: PSK, device: undefined })
    expect([...out, ...err].join('\n')).not.toContain(PSK)
  })

  it('is for this computer only, validates its input, and survives a handler that throws', async () => {
    const base = await boot(async () => { throw new Error(`boom ${PSK}`) })
    expect((await post(base, { op: 'forget' }, {})).status).toBe(403)
    expect((await post(base, { op: 'wat' })).status).toBe(400)
    expect((await post(base, { op: 'set', ssid: 'Home' })).status).toBe(400)
    expect((await post(base, { op: 'set', ssid: 'Home', psk: 5 })).status).toBe(400)
    const res = await post(base, { op: 'set', ssid: 'Home', psk: PSK })
    expect(res.status).toBe(500)
    expect(await res.text()).not.toContain(PSK)
    expect((await fetch(`${base}/api/dial/wifi`)).status).toBe(403)
  })

  it('is unavailable, not a crash, when the daemon has no dial service', async () => {
    const base = await boot(undefined)
    expect((await post(base, { op: 'forget' })).status).toBe(503)
  })
})
