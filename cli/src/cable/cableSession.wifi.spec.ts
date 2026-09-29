// WiFi as the dial's second transport: what the session welcomes, what it refuses, and what it never says.
import { mkdtempSync } from 'node:fs'
import { createServer, type Server } from 'node:net'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableSession, type CableHost, type CablePort, type DialStatus } from './cableSession.js'
import { bindTokenForTcp, bindTokenForUsb, setDialBindDirForTest } from './dialBind.js'
import { DialLog } from './dialLog.js'
import { TcpLink } from './tcpLink.js'

const MAC = 'AA:BB:CC:DD:EE:01'
const PSK = 'not-a-real-password-123'

class Loopback implements CablePort {
  isOpen = true
  sent: Array<Record<string, unknown>> = []
  closedWith: string | null = null
  private decoder = new CableDecoder()
  constructor(readonly path: string, private onData: (chunk: Buffer) => void, private onClosed: (why: string) => void) {}
  async write(bytes: Uint8Array) {
    this.decoder.feed(Buffer.from(bytes), f => {
      if (f.type === CableType.Json) this.sent.push(JSON.parse(Buffer.from(f.payload).toString('utf8')))
    })
  }
  async close(why = 'closed') {
    if (!this.isOpen) return
    this.isOpen = false
    this.closedWith = why
    this.onClosed(why)
  }
  say(msg: Record<string, unknown>) { this.onData(Buffer.from(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(msg))))) }
  types() { return this.sent.map(m => m.t as string) }
}

function makeHost() {
  const lines: string[] = []
  const statuses: DialStatus[] = []
  const host: CableHost = {
    localMachine: () => ({ id: 'local', name: 'Fixture' }),
    listMachines: async () => ({ machines: [], source: 'backend' as const }),
    selectedMachine: () => 'local', selectMachine: async () => ({ ok: true as const }),
    listSwarms: () => ({ selected: '', swarms: [], tiles: [] }), listUnread: () => [],
    selectSwarm: vi.fn(), appName: () => 'harness', voiceLang: () => 'en',
    listAgents: async () => [], agentTotal: () => 0, activeSwarm: () => 't1', describe: () => undefined,
    sendTurn: vi.fn(), stopTurn: vi.fn(), scrolled: vi.fn(), answer: vi.fn(), focus: vi.fn(),
    openAgent: vi.fn(), forkAgent: async id => ({ ok: true as const, agentId: `${id}-fork` }), updateAgent: vi.fn(),
    listModels: async () => [], recentSummaries: async () => [], transcribe: async () => '',
    route: async () => ({ agentId: '', confidence: 0, reason: '' }),
    log: line => { lines.push(line) }, onDialStatus: s => { statuses.push(s) },
  }
  return { host, lines, statuses }
}

async function connect(path: string, options?: { expectMac?: string }) {
  const h = makeHost()
  let port!: Loopback
  const session = new CableSession(h.host, new DialLog(mkdtempSync(join(tmpdir(), 'cable-'))), async (onData, onClosed) => {
    port = new Loopback(path, onData, onClosed)
    return port
  }, options)
  session.start()
  await vi.waitFor(() => expect(port).toBeDefined())
  return { session, port, ...h }
}

const hello = (extra: Record<string, unknown> = {}) => ({ t: 'hello', product: 'harness', mac: MAC, fw: '1.0.0', proto: 3, ...extra })

describe('dial WiFi: the session', () => {
  beforeEach(() => setDialBindDirForTest(mkdtempSync(join(tmpdir(), 'bind-'))))
  afterEach(() => setDialBindDirForTest(undefined))

  it('welcomes a USB dial with a token, minted the first time and the same every time after', async () => {
    const { session, port } = await connect('/dev/loopback')
    try {
      expect(bindTokenForTcp(MAC)).toBeNull()
      port.say(hello())
      await vi.waitFor(() => expect(port.types()).toContain('welcome'))
      const bind = port.sent.find(m => m.t === 'welcome')!.bind
      expect(bind).toMatch(/^[0-9a-f]{64}$/)
      expect(bindTokenForTcp(MAC)).toBe(bind)
      port.say(hello())
      await vi.waitFor(() => expect(port.sent.filter(m => m.t === 'welcome')).toHaveLength(2))
      expect(port.sent.filter(m => m.t === 'welcome')[1].bind).toBe(bind)
    } finally { await session.stop() }
  })

  it('does not welcome a WiFi dial this computer never met over USB, and says why', async () => {
    const { session, port, lines } = await connect('tcp:192.0.2.7:17420')
    try {
      port.say(hello())
      await vi.waitFor(() => expect(port.isOpen).toBe(false))
      expect(port.types()).not.toContain('welcome')
      expect(lines.some(l => l.includes('not USB-paired'))).toBe(true)
      expect(bindTokenForTcp(MAC)).toBeNull()
    } finally { await session.stop() }
  })

  it('welcomes a WiFi dial it holds a token for, with that token, and reports it on WiFi', async () => {
    const token = bindTokenForUsb(MAC)
    const { session, port, statuses } = await connect('tcp:192.0.2.7:17420', { expectMac: MAC })
    try {
      port.say(hello())
      await vi.waitFor(() => expect(port.types()).toContain('welcome'))
      expect(port.sent.find(m => m.t === 'welcome')!.bind).toBe(token)
      await vi.waitFor(() => expect(statuses.at(-1)).toMatchObject({ attached: true, mac: MAC, transport: 'tcp' }))
    } finally { await session.stop() }
  })

  it('will not hand its token to a WiFi peer that is not the dial it looked for', async () => {
    bindTokenForUsb('AA:BB:CC:DD:EE:99')
    bindTokenForUsb(MAC)
    const { session, port } = await connect('tcp:192.0.2.7:17420', { expectMac: 'AA:BB:CC:DD:EE:99' })
    try {
      port.say(hello())
      await vi.waitFor(() => expect(port.isOpen).toBe(false))
      expect(port.types()).not.toContain('welcome')
    } finally { await session.stop() }
  })

  it('never puts wifi.set or wifi.forget on a WiFi link, whoever asks', async () => {
    bindTokenForUsb(MAC)
    const { session, port, statuses } = await connect('tcp:192.0.2.7:17420')
    try {
      port.say(hello())
      await vi.waitFor(() => expect(statuses.some(s => s.attached)).toBe(true))
      expect(await session.setWifi('Home', PSK)).toEqual({ ok: false, error: expect.stringContaining('USB') })
      expect((await session.forgetWifi()).ok).toBe(false)
      // The guard is in the send itself, not only in the two methods.
      expect(await (session as unknown as { send(m: object): Promise<boolean> }).send({ t: 'wifi.set', ssid: 'Home', psk: PSK })).toBe(false)
      expect(await (session as unknown as { send(m: object): Promise<boolean> }).send({ t: 'wifi.forget' })).toBe(false)
      expect(port.types().filter(t => t.startsWith('wifi.'))).toEqual([])
    } finally { await session.stop() }
  })

  it('sends wifi.set and wifi.forget over the cable, and refuses credentials the device would', async () => {
    const { session, port, lines, statuses } = await connect('/dev/loopback')
    try {
      expect((await session.setWifi('Home', PSK)).ok).toBe(false) // not greeted yet
      port.say(hello())
      await vi.waitFor(() => expect(statuses.some(s => s.attached)).toBe(true))
      expect(await session.setWifi('Home', PSK)).toEqual({ ok: true })
      expect(port.sent.find(m => m.t === 'wifi.set')).toEqual({ t: 'wifi.set', ssid: 'Home', psk: PSK })
      expect(await session.setWifi('Open', '')).toEqual({ ok: true })
      expect((await session.setWifi('', PSK)).ok).toBe(false)
      expect((await session.setWifi('x'.repeat(33), PSK)).ok).toBe(false)
      expect((await session.setWifi('Home', 'p'.repeat(64))).ok).toBe(false)
      expect(port.sent.filter(m => m.t === 'wifi.set')).toHaveLength(2)
      expect(await session.forgetWifi()).toEqual({ ok: true })
      expect(port.types()).toContain('wifi.forget')
      // The password is on the wire to the device and nowhere else.
      expect(JSON.stringify(statuses)).not.toContain(PSK)
      expect(lines.join('\n')).not.toContain(PSK)
    } finally { await session.stop() }
  })

  it('reads the wifi state from hello and from wifi.status, and keeps it when unplugged', async () => {
    const { session, port, statuses } = await connect('/dev/loopback')
    try {
      port.say(hello({ wifi: { state: 'connecting', ssid: 'Home' } }))
      await vi.waitFor(() => expect(statuses.at(-1)).toMatchObject({ attached: true, wifi: { state: 'connecting', ssid: 'Home' } }))
      port.say({ t: 'wifi.status', state: 'connected', ssid: 'Home', rssi: -52, ip: '192.0.2.9' })
      await vi.waitFor(() => expect(statuses.at(-1)?.wifi).toEqual({ state: 'connected', ssid: 'Home', rssi: -52, ip: '192.0.2.9' }))
      port.say({ t: 'wifi.status', state: 'failed', ssid: 'Home', reason: 'auth' })
      await vi.waitFor(() => expect(statuses.at(-1)?.wifi).toEqual({ state: 'failed', ssid: 'Home', reason: 'auth' }))
      port.say({ t: 'wifi.status', state: 'nonsense' })
      port.say({ t: 'wifi.status' })
      await new Promise(r => setTimeout(r, 10))
      expect(statuses.at(-1)?.wifi?.state).toBe('failed')
      await port.close('unplugged')
      expect(statuses.at(-1)).toMatchObject({ attached: false, mac: MAC, wifi: { state: 'failed' } })
    } finally { await session.stop() }
  })

  it('a status from a firmware without wifi is unchanged: no wifi field at all', async () => {
    const { session, port, statuses } = await connect('/dev/loopback')
    try {
      port.say(hello())
      await vi.waitFor(() => expect(statuses.length).toBeGreaterThan(0))
      expect(statuses.every(s => !('wifi' in s) && !('transport' in s))).toBe(true)
    } finally { await session.stop() }
  })

  it('speaks over a real TCP socket: the dial end gets a welcome carrying the token', async () => {
    const token = bindTokenForUsb(MAC)
    const decoder = new CableDecoder()
    const got: Array<Record<string, unknown>> = []
    const server: Server = createServer(sock => {
      sock.on('data', d => decoder.feed(d, f => { if (f.type === CableType.Json) got.push(JSON.parse(Buffer.from(f.payload).toString())) }))
      sock.write(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(hello()))))
    })
    await new Promise<void>(r => server.listen(0, '127.0.0.1', r))
    const address = server.address() as { port: number }
    const h = makeHost()
    const session = new CableSession(h.host, new DialLog(mkdtempSync(join(tmpdir(), 'cable-'))),
      (onData, onClosed) => TcpLink.open('127.0.0.1', address.port, onData, onClosed), { expectMac: MAC })
    try {
      session.start()
      await vi.waitFor(() => expect(got.some(m => m.t === 'welcome')).toBe(true))
      expect(got.find(m => m.t === 'welcome')!.bind).toBe(token)
    } finally { await session.stop(); server.close() }
  })
})
