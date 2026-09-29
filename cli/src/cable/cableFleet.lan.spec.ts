// The fleet's WiFi half: which dials it goes looking for, who wins when a dial is on both links, and
// that nothing on the LAN is ever opened that this computer did not USB-pair first.
import { mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { CableFleet } from './cableFleet.js'
import { CableSession, type CableHost, type CablePort } from './cableSession.js'
import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { bindTokenForUsb, setDialBindDirForTest } from './dialBind.js'
import { DialLog } from './dialLog.js'
import { DialVerdicts } from './dialPortVerdicts.js'
import type { DialBrowser } from './tcpLink.js'
import type { DialPort } from './serial.js'

const A = 'AA:BB:CC:DD:EE:01'
const B = 'AA:BB:CC:DD:EE:02'
const usbPort = (serial: string): DialPort => ({ path: `/dev/usb-${serial}`, serialNumber: serial, vendorId: 0x303a, productId: 0x1001 })

class Peer implements CablePort {
  isOpen = true
  sent: Record<string, unknown>[] = []
  private decoder = new CableDecoder()
  constructor(readonly path: string, private onData: (chunk: Buffer) => void, private onClosed: (why: string) => void) {}
  async write(bytes: Uint8Array) { this.decoder.feed(bytes, f => {
    if (f.type === CableType.Json) this.sent.push(JSON.parse(Buffer.from(f.payload).toString()))
  }) }
  async close(why = 'closed') { if (this.isOpen) { this.isOpen = false; this.onClosed(why) } }
  say(m: Record<string, unknown>) { this.onData(Buffer.from(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(m))))) }
  hello(mac: string, extra: Record<string, unknown> = {}) { this.say({ t: 'hello', product: 'harness', mac, fw: 'fixture', proto: 3, ...extra }) }
}

function fixture(opts: { serials?: string[]; paired?: string[]; retryMs?: number } = {}) {
  let usb: DialPort[] = []
  let paired = opts.paired ?? []
  const known = new Map<string, { host: string; port: number }>()
  const usbPeers: Peer[] = []
  const lanPeers: Peer[] = []
  const lines: string[] = []
  let browseFails = false
  const browsers: { stopped: boolean }[] = []
  const browse = vi.fn((): DialBrowser => {
    if (browseFails) throw new Error('no route')
    const b = { stopped: false }
    browsers.push(b)
    return { find: mac => known.get(mac), stop: () => { b.stopped = true } }
  })
  const lanOpen = vi.fn(async (host: string, port: number, onData: (c: Buffer) => void, onClosed: (w: string) => void) => {
    const p = new Peer(`tcp:${host}:${port}`, onData, onClosed); lanPeers.push(p); return p
  })
  const host: CableHost = {
    localMachine: () => ({ id: 'local', name: 'Fixture' }),
    listMachines: async () => ({ machines: [], source: 'backend' }),
    selectedMachine: () => 'local', selectMachine: async () => ({ ok: true }),
    listSwarms: () => ({ selected: 'tab', swarms: [], tiles: [] }), listUnread: () => [],
    selectSwarm: vi.fn(), appName: () => 'harness', voiceLang: () => 'en',
    listAgents: async () => [{ id: 'a', name: 'A', engine: 'codex' }], agentTotal: () => 1,
    activeSwarm: () => 'tab', describe: () => ({ name: 'A', engine: 'codex', machine: 'local' }),
    sendTurn: vi.fn(), stopTurn: vi.fn(), scrolled: vi.fn(), answer: vi.fn(), focus: vi.fn(),
    openAgent: vi.fn(), forkAgent: async () => ({ ok: true, agentId: 'fork' }), updateAgent: vi.fn(),
    listModels: async () => [], recentSummaries: async () => [],
    transcribe: vi.fn(async () => ''), route: async () => ({ agentId: 'a', confidence: 1, reason: 'fixture' }),
    log: line => { lines.push(line) },
    onDialAttached: vi.fn(), onDialGone: vi.fn(), onDialStatus: vi.fn(),
  }
  const verdicts = new DialVerdicts()
  const fleet = new CableFleet(CableSession, host, mkdtempSync(join(tmpdir(), 'lan-fleet-')), DialLog, {
    discover: async () => usb, intervalMs: 60_000, verdicts, serials: opts.serials, inUse: async () => false,
    open: async (path, onData, onClosed) => { const p = new Peer(path, onData, onClosed); usbPeers.push(p); return p },
    lan: { paired: () => paired, browse, open: lanOpen, retryMs: opts.retryMs ?? 60_000 },
  })
  const scan = async () => { await (fleet as unknown as { scan(): Promise<void> }).scan(); await new Promise(r => setTimeout(r, 5)) }
  return {
    fleet, host, verdicts, usbPeers, lanPeers, lines, browse, browsers, lanOpen, scan,
    setUsb: (p: DialPort[]) => { usb = p },
    setPaired: (p: string[]) => { paired = p },
    advertise: (mac: string) => known.set(mac, { host: '192.0.2.10', port: 17420 }),
    unadvertise: (mac: string) => known.delete(mac),
    failBrowse: (v: boolean) => { browseFails = v },
    ids: () => [...(fleet as unknown as { entries: Map<string, { via: string }> }).entries].map(([id, e]) => `${e.via}:${id}`).sort(),
    async start() { fleet.start(); await new Promise(r => setTimeout(r, 5)) },
  }
}

describe('CableFleet over WiFi', () => {
  let f: ReturnType<typeof fixture>
  beforeEach(() => setDialBindDirForTest(mkdtempSync(join(tmpdir(), 'bind-'))))
  afterEach(async () => { await f?.fleet.stop(); setDialBindDirForTest(undefined) })

  it('is silent with nothing paired: no discovery, no connection', async () => {
    f = fixture({ paired: [] })
    f.advertise(A)
    await f.start(); await f.scan(); await f.scan()
    expect(f.browse).not.toHaveBeenCalled()
    expect(f.lanOpen).not.toHaveBeenCalled()
    expect(f.ids()).toEqual([])
  })

  it('connects only to a dial it holds a token for, welcomes it with that token, and lists it', async () => {
    const token = bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    f.advertise(A); f.advertise(B) // B is a stranger on the LAN
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
    expect(f.lanOpen).toHaveBeenCalledTimes(1)
    expect(f.lanOpen).toHaveBeenCalledWith('192.0.2.10', 17420, expect.any(Function), expect.any(Function))
    f.lanPeers[0].hello(A)
    await vi.waitFor(() => expect(f.lanPeers[0].sent.find(m => m.t === 'welcome')?.bind).toBe(token))
    await vi.waitFor(() => expect(f.fleet.devices()).toEqual([expect.objectContaining({ id: A, attached: true, transport: 'tcp' })]))
    expect(f.ids()).toEqual([`lan:${A}`])
  })

  it('waits for mDNS: a paired dial nobody has advertised is not connected to', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    await f.start(); await f.scan()
    expect(f.browse).toHaveBeenCalledTimes(1)
    expect(f.lanOpen).not.toHaveBeenCalled()
    f.advertise(A); await f.scan()
    await vi.waitFor(() => expect(f.lanOpen).toHaveBeenCalledTimes(1))
  })

  it('USB wins: with the cable present there is no WiFi session and no discovery at all', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    f.advertise(A); f.setUsb([usbPort(A)])
    await f.start(); await f.scan(); await f.scan()
    await vi.waitFor(() => expect(f.usbPeers).toHaveLength(1))
    expect(f.lanOpen).not.toHaveBeenCalled()
    expect(f.browse).not.toHaveBeenCalled()
    expect(f.ids()).toEqual([`usb:${A}`])
  })

  it('hands over both ways with one entry per dial: cable out, WiFi takes over; cable back, WiFi stops', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    f.advertise(A); f.setUsb([usbPort(A)])
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.usbPeers).toHaveLength(1))
    f.usbPeers[0].hello(A)
    await vi.waitFor(() => expect(f.fleet.devices()).toHaveLength(1))

    f.setUsb([]); await f.scan(); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
    expect(f.usbPeers[0].isOpen).toBe(false)
    expect(f.ids()).toEqual([`lan:${A}`])
    f.lanPeers[0].hello(A)
    await vi.waitFor(() => expect(f.fleet.devices()).toEqual([expect.objectContaining({ id: A, attached: true, transport: 'tcp' })]))

    f.setUsb([usbPort(A)]); await f.scan()
    await vi.waitFor(() => expect(f.usbPeers).toHaveLength(2))
    expect(f.lanPeers[0].isOpen).toBe(false)
    expect(f.ids()).toEqual([`usb:${A}`])
    for (let i = 0; i < 3; i++) await f.scan()
    expect(f.lanPeers).toHaveLength(1)
    expect(f.browsers.at(-1)?.stopped).toBe(true) // nothing left to look for, so nothing is listening
  })

  it('a cable that is present but held by another program does not end a working WiFi session', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    const inUse = vi.fn(async () => true)
    ;(f.fleet as unknown as { inUse: typeof inUse }).inUse = inUse
    f.advertise(A); f.setUsb([usbPort(A)])
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
    await f.scan()
    expect(f.lanPeers[0].isOpen).toBe(true)
    expect(f.usbPeers).toHaveLength(0)
  })

  it('honours the serial allow-list for WiFi too', async () => {
    bindTokenForUsb(A); bindTokenForUsb(B)
    f = fixture({ paired: [A, B], serials: [B] })
    f.advertise(A); f.advertise(B)
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
    expect(f.ids()).toEqual([`lan:${B}`])
  })

  it('a WiFi peer that is not a Harness dial is dropped without a verdict and without touching the USB verdicts', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    f.advertise(A)
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
    f.lanPeers[0].say({ t: 'hello', product: 'other', mac: A })
    await vi.waitFor(() => expect(f.lanPeers[0].isOpen).toBe(false))
    expect(f.lines.some(l => l.includes('is not a Harness dial'))).toBe(false)
    expect(f.verdicts.isForeign(usbPort(A))).toBe(false)
    expect(f.ids()).toEqual([`lan:${A}`])
  })

  it('stops the WiFi session when the pairing is gone', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    f.advertise(A)
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
    f.setPaired([]); await f.scan()
    expect(f.lanPeers[0].isOpen).toBe(false)
    expect(f.ids()).toEqual([])
    expect(f.browsers.at(-1)?.stopped).toBe(true)
  })

  it('says once, and tries again later, when there is no network to browse', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A], retryMs: 0 })
    f.failBrowse(true)
    await f.start(); await f.scan(); await f.scan(); await f.scan()
    expect(f.lines.filter(l => l.includes('WiFi discovery is not available'))).toHaveLength(1)
    f.failBrowse(false); f.advertise(A); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
  })

  it('a dial that will not take the connection is reported once, not every retry', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    f.lanOpen.mockRejectedValue(new Error('ECONNREFUSED'))
    f.advertise(A)
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.lines.some(l => l.includes('cannot reach'))).toBe(true))
    await new Promise(r => setTimeout(r, 2500))
    expect(f.lines.filter(l => l.includes('cannot reach'))).toHaveLength(1)
  })
})

describe('CableFleet WiFi settings', () => {
  let f: ReturnType<typeof fixture>
  beforeEach(() => setDialBindDirForTest(mkdtempSync(join(tmpdir(), 'bind-'))))
  afterEach(async () => { await f?.fleet.stop(); setDialBindDirForTest(undefined) })
  const greet = async (peer: Peer, mac: string) => { peer.hello(mac); await vi.waitFor(() => expect(peer.sent.some(m => m.t === 'welcome')).toBe(true)); await new Promise(r => setTimeout(r, 5)) }

  it('defaults to the only USB dial, names one with a device, and refuses several without one', async () => {
    f = fixture()
    f.setUsb([usbPort(A)])
    await f.start()
    await vi.waitFor(() => expect(f.usbPeers).toHaveLength(1))
    await greet(f.usbPeers[0], A)
    expect(await f.fleet.setWifi(undefined, 'Home', 'fake-psk')).toMatchObject({ ok: true, id: A })
    expect(f.usbPeers[0].sent.find(m => m.t === 'wifi.set')).toEqual({ t: 'wifi.set', ssid: 'Home', psk: 'fake-psk' })
    expect(await f.fleet.forgetWifi(A.toLowerCase())).toMatchObject({ ok: true })
    expect(await f.fleet.setWifi('nope', 'Home', 'x')).toMatchObject({ ok: false })

    f.setUsb([usbPort(A), usbPort(B)]); await f.scan()
    await vi.waitFor(() => expect(f.usbPeers).toHaveLength(2))
    await greet(f.usbPeers[1], B)
    const many = await f.fleet.setWifi(undefined, 'Home', 'x')
    expect(many.ok).toBe(false)
    expect(many.error).toContain('--device')
    expect(await f.fleet.setWifi(B, 'Home', 'x')).toMatchObject({ ok: true, id: B })
    expect(f.usbPeers[1].sent.filter(m => m.t === 'wifi.set')).toHaveLength(1)
    expect(f.usbPeers[0].sent.filter(m => m.t === 'wifi.set')).toHaveLength(1)
  })

  it('refuses when no dial is on a cable, including one that is only reachable over WiFi', async () => {
    bindTokenForUsb(A)
    f = fixture({ paired: [A] })
    f.advertise(A)
    await f.start(); await f.scan()
    await vi.waitFor(() => expect(f.lanPeers).toHaveLength(1))
    await greet(f.lanPeers[0], A)
    expect((await f.fleet.setWifi(undefined, 'Home', 'x')).error).toContain('USB')
    expect((await f.fleet.setWifi(A, 'Home', 'x')).error).toContain('USB')
    expect((await f.fleet.forgetWifi()).ok).toBe(false)
    expect(f.lanPeers[0].sent.filter(m => String(m.t).startsWith('wifi.'))).toEqual([])
  })

  it('reports each dial\'s wifi state, whichever link it is on', async () => {
    f = fixture()
    f.setUsb([usbPort(A)])
    await f.start()
    await vi.waitFor(() => expect(f.usbPeers).toHaveLength(1))
    await greet(f.usbPeers[0], A)
    f.usbPeers[0].say({ t: 'wifi.status', state: 'connected', ssid: 'Home', ip: '192.0.2.9' })
    await vi.waitFor(() => expect(f.fleet.wifiStatus()).toEqual([
      { id: A, mac: A, attached: true, transport: undefined, wifi: { state: 'connected', ssid: 'Home', ip: '192.0.2.9' } }]))
  })
})
