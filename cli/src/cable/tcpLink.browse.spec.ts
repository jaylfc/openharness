import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { browseDials } from './tcpLink.js'

type Handler = (service: Record<string, unknown>) => void
/** A network that says only what a test tells it to, and counts how often it was asked. */
function fakeNetwork() {
  const handlers: Record<string, Handler[]> = { up: [], down: [] }
  let starts = 0, stops = 0
  const factory = () => ({
    find: () => {
      starts++
      handlers.up = []; handlers.down = []
      return { on: (e: 'up' | 'down', fn: Handler) => { handlers[e].push(fn) }, stop: () => { stops++ } }
    },
    destroy: () => {},
  })
  return { factory, up: (s: Record<string, unknown>) => handlers.up.forEach(f => f(s)), down: (s: Record<string, unknown>) => handlers.down.forEach(f => f(s)),
    starts: () => starts, stops: () => stops }
}

const dial = { name: 'harness-4F98', host: 'harness-4F98.local', port: 17420, addresses: ['192.0.2.7'], txt: { mac: '7c:0c:5f:42:4f:98' } }

describe('browsing the network for dials', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers() })

  it('finds a dial by its advertised MAC, whatever the case', () => {
    const net = fakeNetwork(), log = vi.fn()
    const b = browseDials(undefined, { bonjour: net.factory, log })
    net.up(dial)
    expect(b.find('7C:0C:5F:42:4F:98')).toEqual({ host: '192.0.2.7', port: 17420 })
    expect(log).toHaveBeenCalledWith(expect.stringContaining('a dial is on the network: 7C:0C:5F:42:4F:98'))
    b.stop()
  })

  it('reads a MAC that arrives as bytes', () => {
    const net = fakeNetwork()
    const b = browseDials(undefined, { bonjour: net.factory })
    net.up({ ...dial, txt: { mac: Buffer.from('7C:0C:5F:42:4F:98') } })
    expect(b.find('7C:0C:5F:42:4F:98')).toBeDefined()
    b.stop()
  })

  it('finds a dial whose TXT record was dropped, by the name it also carries', () => {
    const net = fakeNetwork()
    const b = browseDials(undefined, { bonjour: net.factory })
    net.up({ ...dial, txt: {} })
    expect(b.find('7C:0C:5F:42:4F:98')).toEqual({ host: '192.0.2.7', port: 17420 })
    expect(b.find('7C:0C:5F:42:00:00')).toBeUndefined()   // another dial's tag is not this one
    b.stop()
  })

  it('falls back to the dial\'s own name when no address arrived', () => {
    const net = fakeNetwork()
    const b = browseDials(undefined, { bonjour: net.factory })
    net.up({ ...dial, addresses: [] })
    expect(b.find('7C:0C:5F:42:4F:98')).toEqual({ host: 'harness-4F98.local', port: 17420 })
    b.stop()
  })

  it('forgets a dial that says goodbye', () => {
    const net = fakeNetwork(), log = vi.fn()
    const b = browseDials(undefined, { bonjour: net.factory, log })
    net.up(dial); net.down(dial)
    expect(b.find('7C:0C:5F:42:4F:98')).toBeUndefined()
    expect(log).toHaveBeenCalledWith(expect.stringContaining('a dial left the network'))
    b.stop()
  })

  it('asks afresh every round, and forgets a dial that stopped answering', () => {
    const net = fakeNetwork()
    const b = browseDials(undefined, { bonjour: net.factory, refreshMs: 1000 })
    net.up(dial)
    expect(net.starts()).toBe(1)
    vi.advanceTimersByTime(1000)   // a round: asked again; the dial answers
    expect(net.starts()).toBe(2)
    net.up(dial)
    vi.advanceTimersByTime(1000)
    net.up(dial)
    expect(b.find('7C:0C:5F:42:4F:98')).toBeDefined()
    // Powered off with no goodbye: not heard for more than two rounds.
    vi.advanceTimersByTime(3000)
    expect(b.find('7C:0C:5F:42:4F:98')).toBeUndefined()
    b.stop()
  })

  it('stops asking when told to', () => {
    const net = fakeNetwork()
    const b = browseDials(undefined, { bonjour: net.factory, refreshMs: 1000 })
    b.stop()
    const before = net.starts()
    vi.advanceTimersByTime(5000)
    expect(net.starts()).toBe(before)
  })
})
