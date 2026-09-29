import { EventEmitter } from 'node:events'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { browseDialsDnsSd, type SpawnLike } from './dnsSdBrowse.js'

/** A `dns-sd` that prints only what a test feeds it. */
class Fake extends EventEmitter {
  stdout = new EventEmitter() as EventEmitter & { on(e: 'data', f: (c: Buffer | string) => void): unknown }
  killed = false
  constructor(readonly args: string[]) { super() }
  kill() { this.killed = true; this.emit('exit') }
  print(text: string) { this.stdout.emit('data', Buffer.from(text)) }
}
function network() {
  const procs: Fake[] = []
  const spawn: SpawnLike = (_cmd, args) => { const p = new Fake(args); procs.push(p); return p as never }
  return { spawn, procs, browse: () => procs.find(p => p.args[0] === '-B')!, lookups: () => procs.filter(p => p.args[0] === '-L') }
}

const BROWSE_HEADER = 'Browsing for _harness-dial._tcp.local.\nDATE: ---Tue 29 Sep 2026---\n17:50:01.123  ...STARTING...\nTimestamp     A/R    Flags  if Domain               Service Type         Instance Name\n'
const add = (name: string) => `17:50:01.456  Add        2  16 local.               _harness-dial._tcp.  ${name}\n`
const rmv = (name: string) => `17:59:01.456  Rmv        0  16 local.               _harness-dial._tcp.  ${name}\n`
const reachable = (name: string, host = 'harness-4F98.local.', port = 17420) =>
  `Lookup ${name}._harness-dial._tcp.local\nDATE: ---Tue 29 Sep 2026---\n17:50:01.789  ${name}._harness-dial._tcp.local. can be reached at ${host}:${port} (interface 16)\n`

describe('browsing through the system resolver', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers() })

  it('finds a dial by the MAC in its TXT, and connects to its .local name', () => {
    const net = network(), log = vi.fn()
    const b = browseDialsDnsSd(() => {}, { spawn: net.spawn, log })
    net.browse().print(BROWSE_HEADER + add('OpenHarness dial'))
    expect(net.lookups()).toHaveLength(1)
    expect(net.lookups()[0].args).toEqual(['-L', 'OpenHarness dial', '_harness-dial._tcp', 'local.'])
    net.lookups()[0].print(reachable('OpenHarness dial') + ' mac=7C:0C:5F:42:4F:98\n')
    expect(b.find('7c:0c:5f:42:4f:98')).toEqual({ host: 'harness-4F98.local', port: 17420 })
    expect(log).toHaveBeenCalledWith(expect.stringContaining('a dial is on the network: 7C:0C:5F:42:4F:98 at harness-4F98.local:17420'))
    expect(net.lookups()[0].killed).toBe(true)   // one answer is all it wanted
    b.stop()
  })

  it('reads output that arrives in pieces, mid-line', () => {
    const net = network()
    const b = browseDialsDnsSd(() => {}, { spawn: net.spawn })
    const text = BROWSE_HEADER + add('OpenHarness dial')
    net.browse().print(text.slice(0, 200)); net.browse().print(text.slice(200))
    expect(net.lookups()).toHaveLength(1)
    b.stop()
  })

  it('keeps a dial whose TXT never comes, matched by the harness-XXXX host name', () => {
    const net = network()
    const b = browseDialsDnsSd(() => {}, { spawn: net.spawn })
    net.browse().print(add('OpenHarness dial'))
    net.lookups()[0].print(reachable('OpenHarness dial'))
    vi.advanceTimersByTime(5000)   // gives up waiting for a TXT line
    expect(b.find('7C:0C:5F:42:4F:98')).toEqual({ host: 'harness-4F98.local', port: 17420 })
    expect(b.find('7C:0C:5F:42:00:00')).toBeUndefined()
    b.stop()
  })

  it('handles two dials with instance names that contain spaces and numbers', () => {
    const net = network()
    const b = browseDialsDnsSd(() => {}, { spawn: net.spawn })
    net.browse().print(add('OpenHarness dial') + add('OpenHarness dial (2)'))
    expect(net.lookups().map(p => p.args[1])).toEqual(['OpenHarness dial', 'OpenHarness dial (2)'])
    net.lookups()[0].print(reachable('OpenHarness dial', 'harness-4F98.local.') + ' mac=7C:0C:5F:42:4F:98\n')
    net.lookups()[1].print(reachable('OpenHarness dial (2)', 'harness-EC3C.local.') + ' mac=F4:12:FA:84:EC:3C\n')
    expect(b.find('7C:0C:5F:42:4F:98')?.host).toBe('harness-4F98.local')
    expect(b.find('F4:12:FA:84:EC:3C')?.host).toBe('harness-EC3C.local')
    b.stop()
  })

  it('forgets a dial that leaves', () => {
    const net = network(), log = vi.fn()
    const b = browseDialsDnsSd(() => {}, { spawn: net.spawn, log })
    net.browse().print(add('OpenHarness dial'))
    net.lookups()[0].print(reachable('OpenHarness dial') + ' mac=7C:0C:5F:42:4F:98\n')
    net.browse().print(rmv('OpenHarness dial'))
    expect(b.find('7C:0C:5F:42:4F:98')).toBeUndefined()
    expect(log).toHaveBeenCalledWith(expect.stringContaining('a dial left the network'))
    b.stop()
  })

  it('forgets a dial that stopped answering without a goodbye, and re-reads the ones that still do', () => {
    const net = network()
    const b = browseDialsDnsSd(() => {}, { spawn: net.spawn, refreshMs: 1000 })
    net.browse().print(add('OpenHarness dial'))
    net.lookups()[0].print(reachable('OpenHarness dial') + ' mac=7C:0C:5F:42:4F:98\n')
    vi.advanceTimersByTime(1000)
    expect(net.lookups().length).toBe(2)   // asked again on the beat
    net.lookups()[1].print(reachable('OpenHarness dial') + ' mac=7C:0C:5F:42:4F:98\n')
    expect(b.find('7C:0C:5F:42:4F:98')).toBeDefined()
    vi.advanceTimersByTime(3000)           // no more answers: power lost
    expect(b.find('7C:0C:5F:42:4F:98')).toBeUndefined()
    b.stop()
  })

  it('starts the browse again if dns-sd exits, and stops everything when told to', () => {
    const net = network()
    const b = browseDialsDnsSd(() => {}, { spawn: net.spawn })
    const first = net.browse()
    first.emit('exit')
    vi.advanceTimersByTime(5000)
    expect(net.procs.filter(p => p.args[0] === '-B').length).toBe(2)
    b.stop()
    expect(net.procs.filter(p => p.args[0] === '-B').every(p => p.killed || p === first)).toBe(true)
    const before = net.procs.length
    vi.advanceTimersByTime(60000)
    expect(net.procs.length).toBe(before)
  })

  it('says once when dns-sd cannot run', () => {
    const onError = vi.fn()
    const b = browseDialsDnsSd(onError, { spawn: () => { throw new Error('ENOENT') } })
    vi.advanceTimersByTime(60000)
    expect(onError).toHaveBeenCalledTimes(1)
    b.stop()
  })
})
