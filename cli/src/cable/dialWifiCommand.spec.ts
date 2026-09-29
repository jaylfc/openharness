import { describe, expect, it, vi } from 'vitest'
import { dialWifiCommand, type DialWifiDeps } from './dialWifiCommand.js'

const PSK = 'not-a-real-password-123'

function harness(over: Partial<DialWifiDeps> = {}, replies: Array<{ status: number; json: Record<string, unknown> }> = []) {
  const out: string[] = []
  const err: string[] = []
  const calls: Array<{ method: string; path: string; body?: unknown }> = []
  const deps: DialWifiDeps = {
    call: async (method, path, body) => {
      calls.push({ method, path, body })
      return replies.shift() ?? { status: 200, json: { ok: true, devices: [] } }
    },
    promptPassword: vi.fn(async () => PSK),
    isTTY: true, env: {}, output: l => out.push(l), error: l => err.push(l),
    sleep: async () => {}, waitMs: 3,
    ...over,
  }
  return { deps, out, err, calls, text: () => [...out, ...err].join('\n') }
}

describe('harness dial wifi', () => {
  it('asks for the password on a TTY, sends it only in the request body, and prints none of it', async () => {
    const h = harness({}, [
      { status: 200, json: { ok: true, id: 'AA:01' } },
      { status: 200, json: { devices: [{ id: 'AA:01', attached: true, wifi: { state: 'connected', ssid: 'Home', ip: '192.0.2.9' } }] } },
    ])
    expect(await dialWifiCommand(['Home'], h.deps)).toBe(0)
    expect(h.deps.promptPassword).toHaveBeenCalledTimes(1)
    expect(vi.mocked(h.deps.promptPassword).mock.calls[0][0]).not.toContain(PSK)
    expect(h.calls[0]).toEqual({ method: 'POST', path: '/api/dial/wifi', body: { op: 'set', ssid: 'Home', psk: PSK } })
    expect(h.calls.every(c => !c.path.includes(PSK))).toBe(true)
    expect(h.text()).not.toContain(PSK)
    expect(h.text()).toContain('Joined "Home" at 192.0.2.9')
  })

  it('reads HARNESS_WIFI_PSK when there is no TTY, and an empty one means an open network', async () => {
    const withKey = harness({ isTTY: false, env: { HARNESS_WIFI_PSK: PSK } })
    await dialWifiCommand(['Home', '--device', 'AA:01'], withKey.deps)
    expect(withKey.deps.promptPassword).not.toHaveBeenCalled()
    expect(withKey.calls[0].body).toEqual({ op: 'set', ssid: 'Home', psk: PSK, device: 'AA:01' })
    const open = harness({ isTTY: false, env: { HARNESS_WIFI_PSK: '' } })
    await dialWifiCommand(['Cafe', '--device=BB:02'], open.deps)
    expect(open.calls[0].body).toEqual({ op: 'set', ssid: 'Cafe', psk: '', device: 'BB:02' })
    expect(`${withKey.text()}${open.text()}`).not.toContain(PSK)
  })

  it('does not guess when there is no terminal and no HARNESS_WIFI_PSK', async () => {
    const h = harness({ isTTY: false })
    expect(await dialWifiCommand(['Home'], h.deps)).toBe(1)
    expect(h.calls).toEqual([])
    expect(h.err.join('\n')).toContain('HARNESS_WIFI_PSK')
  })

  it('refuses the password as an argument, whatever the flag is called', async () => {
    for (const flag of ['--psk', '--password', '--psk=hunter2', '--pass=hunter2']) {
      const h = harness()
      expect(await dialWifiCommand(['Home', flag, ...(flag.includes('=') ? [] : ['hunter2'])], h.deps)).toBe(1)
      expect(h.calls).toEqual([])
      expect(h.text()).not.toContain('hunter2')
    }
  })

  it('takes exactly one of: a network, --forget, --status', async () => {
    for (const argv of [[], ['A', 'B'], ['A', '--forget'], ['--forget', '--status'], ['--device']]) {
      const h = harness()
      expect(await dialWifiCommand(argv, h.deps), argv.join(' ')).toBe(1)
      expect(h.calls).toEqual([])
    }
  })

  it('forgets and shows status, and says so when the daemon is not running', async () => {
    const forget = harness({}, [{ status: 200, json: { ok: true } }])
    expect(await dialWifiCommand(['--forget', '--device', 'AA:01'], forget.deps)).toBe(0)
    expect(forget.calls[0].body).toEqual({ op: 'forget', device: 'AA:01' })
    const status = harness({}, [{ status: 200, json: { devices: [{ id: 'AA:01', attached: true, wifi: { state: 'failed', ssid: 'Home', reason: 'auth' } }] } }])
    expect(await dialWifiCommand(['--status'], status.deps)).toBe(0)
    expect(status.out.join('\n')).toContain('failed  Home  auth')
    const down = harness({ call: async () => { throw new Error('ECONNREFUSED') } })
    expect(await dialWifiCommand(['--status'], down.deps)).toBe(1)
    expect(down.err.join('\n')).toContain('harness start')
  })

  it('reports the daemon\'s refusal (no USB dial, several dials) without the password', async () => {
    const h = harness({}, [{ status: 409, json: { ok: false, error: 'Several dials are plugged in (A, B). Say which with --device <serial>.' } }])
    expect(await dialWifiCommand(['Home'], h.deps)).toBe(1)
    expect(h.err.join('\n')).toContain('--device')
    expect(h.text()).not.toContain(PSK)
  })

  it('reports a failed join, and a join still pending, without failing the send', async () => {
    const failed = harness({}, [{ status: 200, json: { ok: true, id: 'AA:01' } },
      { status: 200, json: { devices: [{ id: 'AA:01', wifi: { state: 'failed', ssid: 'Home', reason: 'auth' } }] } }])
    expect(await dialWifiCommand(['Home'], failed.deps)).toBe(1)
    expect(failed.err.join('\n')).toContain('(auth)')
    const pending = harness({}, [{ status: 200, json: { ok: true, id: 'AA:01' } },
      { status: 200, json: { devices: [{ id: 'AA:01', wifi: { state: 'connecting', ssid: 'Home' } }] } }])
    expect(await dialWifiCommand(['Home'], pending.deps)).toBe(0)
    expect(pending.out.join('\n')).toContain('--status')
  })
})
