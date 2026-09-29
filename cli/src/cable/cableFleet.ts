// One protocol session per dial, sharing the desktop's event source. The cable is the way in; a dial
// this computer has been plugged into can also be reached over WiFi while its cable is out (`lan`).
// Voice buffers, decoding, firmware transfers and disconnects remain per dial.
import { join } from 'node:path'
import { DialVerdicts } from './dialPortVerdicts.js'
import { findDialPorts, portInUse, SerialLink, type DialPort } from './serial.js'
import { pairedDialMacs } from './dialBind.js'
import { browseDials, TcpLink, type DialBrowser } from './tcpLink.js'
import type { CableSession, CableHost, CablePort, DialStatus, PortOpener } from './cableSession.js'
import type { DialLog } from './dialLog.js'

type Surface = Pick<CableSession, keyof CableSession>
type SessionConstructor = new (host: CableHost, log: DialLog, open: PortOpener, options?: { expectMac?: string }) => Surface
/** `via` is how this entry reaches the dial. A WiFi entry has no port. */
type Entry = { via: 'usb' | 'lan'; label: string; port?: DialPort; session: Surface; attached: boolean; status: DialStatus; lanFailNoted?: boolean }
type OpenPort = (path: string, onData: (chunk: Buffer) => void, onClosed: (why: string) => void) => Promise<CablePort>
/** Reaching dials that hold a USB-minted token over WiFi. Off unless `CableFleetOptions.lan` is given. */
export interface LanOptions {
  /** The MACs this computer holds a bind token for: the only dials ever looked for or connected to. */
  paired?: () => string[]
  /** Start looking for dials on the LAN. May throw when there is no network. */
  browse?: (onError: (why: string) => void) => DialBrowser
  open?: (host: string, port: number, onData: (chunk: Buffer) => void, onClosed: (why: string) => void) => Promise<CablePort>
  /** How long to leave discovery alone after it failed to start. */
  retryMs?: number
}

export interface CableFleetOptions {
  lan?: LanOptions
  discover?: () => Promise<DialPort[]>
  open?: OpenPort
  /** Optional local selection. An empty list discovers all matching dials. */
  serials?: string[]
  intervalMs?: number
  /** Which boards have already been found not to be dials. In memory only unless it has a file. */
  verdicts?: DialVerdicts
  /** Does another process have this tty open? A port somebody else is using is not looked at. */
  inUse?: (path: string) => Promise<boolean>
  /** How often a board ruled out is checked for another program working on it. */
  watchEveryMs?: number
}

export class CableFleet {
  private entries = new Map<string, Entry>()
  private timer?: ReturnType<typeof setInterval>
  private scanTask?: Promise<void>
  private stopped = true
  private readonly discover: () => Promise<DialPort[]>
  private readonly open: OpenPort
  private readonly serials: Set<string>
  private readonly verdicts: DialVerdicts
  private readonly inUse: (path: string) => Promise<boolean>
  /** Ports already reported as in use, so the log says it once rather than every scan. */
  private readonly heldNotes = new Set<string>()
  /** For each board ruled out: when it was last checked for another program's use, and whether it was in use. */
  private readonly watched = new Map<string, { at: number; wasBusy: boolean }>()
  private readonly watchEveryMs: number
  private browser?: DialBrowser
  private browseRetryAt = 0
  private browseDownNoted = false

  constructor(private readonly Session: SessionConstructor, private readonly host: CableHost,
              private readonly logs: string, private readonly Log: typeof DialLog,
              private readonly options: CableFleetOptions = {}) {
    this.discover = options.discover ?? findDialPorts
    this.open = options.open ?? SerialLink.open
    this.serials = new Set((options.serials ?? []).map(s => s.toUpperCase()))
    this.verdicts = options.verdicts ?? new DialVerdicts()
    this.inUse = options.inUse ?? portInUse
    this.watchEveryMs = options.watchEveryMs ?? 8_000
  }

  get isConnected(): boolean { return [...this.entries.values()].some(e => e.session.isConnected) }

  start(): void {
    if (!this.stopped) return
    this.stopped = false
    this.timer = setInterval(() => void this.scan(), this.options.intervalMs ?? 2000)
    void this.scan()
  }

  async stop(): Promise<void> {
    this.stopped = true
    clearInterval(this.timer)
    this.timer = undefined
    await this.scanTask
    await Promise.allSettled([...this.entries.values()].map(e => e.session.stop()))
    this.entries.clear()
    this.stopBrowsing()
  }

  private scan(): Promise<void> {
    if (this.stopped) return Promise.resolve()
    if (this.scanTask) return this.scanTask
    this.scanTask = this.reconcile().catch(error => {
      this.host.log(`cable: USB discovery failed: ${String(error)}`)
    }).finally(() => { this.scanTask = undefined })
    return this.scanTask
  }

  /**
   * Has a program had this ruled-out board open and let go since it was ruled out?
   *
   * That is how a board becomes a dial: someone flashes it, with esptool or `idf.py`, and neither a
   * reset nor a reflash changes what USB says about it, so the flash itself is the only sign there is.
   * When the port goes from held to free the verdict is forgotten and the board is looked at once more.
   * Checked every few seconds, not every scan: `lsof` is a process, and the answer changes slowly.
   */
  private async workedOn(port: DialPort): Promise<boolean> {
    const key = (port.serialNumber ?? port.path).toUpperCase()
    const watch = this.watched.get(key) ?? { at: 0, wasBusy: false }
    this.watched.set(key, watch)
    if (Date.now() - watch.at < this.watchEveryMs) return false
    watch.at = Date.now()
    if (await this.inUse(port.path)) { watch.wasBusy = true; return false }
    if (!watch.wasBusy) return false
    watch.wasBusy = false
    this.verdicts.clear(port)
    this.host.log(`cable: ${port.path} was in use and is free again — looking at it once more`)
    return true
  }

  private async reconcile(): Promise<void> {
    const ports = await this.discover()
    if (this.stopped) return
    const present = new Map<string, DialPort>()
    for (const port of ports) {
      const serial = port.serialNumber?.toUpperCase()
      if (this.serials.size && (!serial || !this.serials.has(serial))) continue
      // Found not to be a dial already, and nobody seen working on it since: nothing to look at.
      if (this.verdicts.isForeign(port) && !(await this.workedOn(port))) continue
      present.set(serial || port.path, port)
    }
    let removed = false
    for (const [id, entry] of this.entries) {
      if (entry.via === 'lan' || present.get(id)?.path === entry.port?.path) continue
      await entry.session.stop()
      this.entries.delete(id)
      removed = true
    }
    // After the deletes, not inside them: a publish mid-loop would name a device this computer has
    // already stopped talking to.
    if (removed) this.publish()
    if (this.stopped) return
    for (const [id, port] of present) {
      const held = this.entries.get(id)
      if (held?.via === 'usb') continue
      // A board somebody else has open is somebody's work in progress (a flash, a monitor, a console).
      // Two readers on one tty interleave bytes, so it is not opened, and looked at again next scan.
      if (await this.inUse(port.path)) {
        if (!this.heldNotes.has(port.path)) this.host.log(`cable: ${port.path} is in use by another program — leaving it alone`)
        this.heldNotes.add(port.path)
        continue
      }
      this.heldNotes.delete(port.path)
      if (this.stopped) return
      // USB wins: the cable is about to be the way in, so the WiFi session for this dial ends first. Only
      // now, not when the port merely appears: a port somebody else has open is not a session.
      if (held) await this.dropEntry(id, held, `USB ${port.path} is back`)
      if (this.stopped) return
      const entry: Entry = { via: 'usb', label: port.path, port, attached: false, status: { attached: false }, session: undefined! }
      this.launch(id, entry, () => async (onData, onClosed) => {
        if (this.stopped || this.entries.get(id) !== entry) return null
        const opened = await this.open(port.path, onData, onClosed)
        if (this.stopped || this.entries.get(id) !== entry) {
          await opened.close('USB dial removed while opening')
          return null
        }
        return opened
      })
    }
    await this.reconcileLan()
  }

  /** Give an entry its session, wired to the shared desktop, and start it. */
  private launch(id: string, entry: Entry, makeOpener: () => PortOpener): void {
    const tag = entry.via === 'usb' ? 'usb' : 'lan'
    const attached = () => {
      if (entry.attached) return
      const first = ![...this.entries.values()].some(e => e.attached)
      entry.attached = true
      if (first) this.host.onDialAttached?.()
    }
    const gone = () => {
      if (!entry.attached) return
      entry.attached = false
      // The session's own {attached:false} carried the mac and the last settings; keep them so the
      // pane can show this device's rows read-only instead of dropping it off the desk.
      entry.status = { ...entry.status, id, attached: false, fw: undefined, updating: undefined }
      if (![...this.entries.values()].some(e => e.attached)) this.host.onDialGone?.()
      this.publish()
    }
    // Bind ordinary host methods to the shared desktop. Only connection life
    // cycle and status are aggregated: losing one dial cannot tear down the
    // remaining dial's cloud lane, selection or active voice context.
    const host = new Proxy(this.host, {
      get: (target, key) => {
        if (key === 'onDialAttached') return attached
        if (key === 'onDialGone') return gone
        if (key === 'onForeignPort') return (path: string, why: string) => {
          // A WiFi dial is one this computer holds a token for: never ruled out, never a stranger.
          if (!entry.port) return
          this.verdicts.markForeign(entry.port)
          this.host.log(`cable: ${path} is not a Harness dial (${why}) — leaving it alone until it is unplugged or reset [usb ${id}]`)
          // Off this call stack: the session is in the middle of deciding this, and stop() waits on it.
          setTimeout(() => {
            if (this.entries.get(id) !== entry) return
            this.entries.delete(id)
            void entry.session.stop()
          }, 0)
        }
        if (key === 'onDialStatus') return (status: DialStatus) => {
          entry.status = { ...status, id }
          this.publish()
        }
        if (key === 'log') return (line: string) => this.host.log(`${line} [${tag} ${id}]`)
        const value = Reflect.get(target, key, target)
        return typeof value === 'function' ? value.bind(target) : value
      },
    })
    const dir = `${tag}-${id.replace(/[^a-zA-Z0-9_-]/g, '-')}`
    entry.session = new this.Session(host, new this.Log(join(this.logs, dir)), makeOpener(),
      entry.via === 'lan' ? { expectMac: id } : undefined)
    this.entries.set(id, entry)
    entry.session.start()
  }

  /** Stop and forget one entry, then say so: a publish mid-way would name a device already let go. */
  private async dropEntry(id: string, entry: Entry, why: string): Promise<void> {
    if (this.entries.get(id) !== entry) return
    this.entries.delete(id)
    if (entry.via === 'lan') this.host.log(`cable: ${why} — dropping the WiFi session [lan ${id}]`)
    await entry.session.stop()
    this.publish()
  }

  /**
   * Dials reached over WiFi: only those this computer holds a USB-minted token for, only while their
   * cable is not in use, and only once mDNS has seen them. With nothing paired, or every paired dial on
   * its cable, nothing is browsing and nothing is opened.
   */
  private async reconcileLan(): Promise<void> {
    const lan = this.options.lan
    if (!lan || this.stopped) return
    const wanted = new Set<string>()
    for (const mac of (lan.paired ?? pairedDialMacs)()) {
      const id = mac.toUpperCase()
      if (this.serials.size && !this.serials.has(id)) continue
      if (this.entries.get(id)?.via === 'usb') continue
      wanted.add(id)
    }
    for (const [id, entry] of [...this.entries]) {
      if (entry.via === 'lan' && !wanted.has(id)) await this.dropEntry(id, entry, 'no longer wanted over WiFi')
    }
    if (!wanted.size) { this.stopBrowsing(); return }
    this.startBrowsing(lan)
    if (!this.browser || this.stopped) return
    for (const id of wanted) {
      if (this.entries.has(id) || !this.browser.find(id)) continue
      const entry: Entry = { via: 'lan', label: `wifi ${id}`, attached: false, status: { attached: false }, session: undefined! }
      this.launch(id, entry, () => async (onData, onClosed) => {
        const address = this.browser?.find(id)
        if (this.stopped || this.entries.get(id) !== entry || !address) return null
        let opened: CablePort
        try {
          opened = await (lan.open ?? TcpLink.open)(address.host, address.port, onData, onClosed)
        } catch (error) {
          // Said once until it works again: a dial that is asleep or busy on its cable is not news.
          if (!entry.lanFailNoted) this.host.log(`cable: cannot reach ${id} over WiFi: ${(error as Error).message} [lan ${id}]`)
          entry.lanFailNoted = true
          return null
        }
        entry.lanFailNoted = false
        if (this.stopped || this.entries.get(id) !== entry) {
          await opened.close('WiFi dial no longer wanted while opening')
          return null
        }
        return opened
      })
    }
  }

  private startBrowsing(lan: LanOptions): void {
    if (this.browser || Date.now() < this.browseRetryAt) return
    try {
      const onError = (why: string) => {
        if (!this.browseDownNoted) this.host.log(`cable: WiFi discovery error: ${why}`)
        this.browseDownNoted = true
      }
      this.browser = lan.browse ? lan.browse(onError) : browseDials(onError, { log: line => this.host.log(line) })
      this.host.log('cable: looking for paired dials on the network')
      this.browseDownNoted = false
    } catch (error) {
      if (!this.browseDownNoted) this.host.log(`cable: WiFi discovery is not available: ${String(error)}`)
      this.browseDownNoted = true
      this.browseRetryAt = Date.now() + (lan.retryMs ?? 60_000)
    }
  }

  private stopBrowsing(): void {
    this.browser?.stop()
    this.browser = undefined
  }

  /**
   * One status out, carrying every device.
   *
   * The flat fields name the device a single-device window would have been shown anyway — the one
   * taking an update if there is one, otherwise the first attached. `devices` is the whole desk, which
   * is what a settings pane needs: it has to say WHICH robot it is changing, and until this existed the
   * fleet picked one row and discarded the rest.
   */
  private publish(): void {
    const devices = [...this.entries.values()].map(e => e.status).filter(s => s.attached || s.settings)
    const live = devices.filter(s => s.attached)
    const primary = live.find(s => s.updating) ?? live[0]
    this.host.onDialStatus?.({ ...(primary ?? { attached: false }), devices })
  }

  /** Every device this computer can see, newest reading. Ordered as they were discovered. */
  devices(): DialStatus[] {
    return [...this.entries.values()].map(e => e.status).filter(s => s.attached || s.settings)
  }

  /**
   * Change one device's settings. Addressed, not broadcast: every other method here reaches every
   * device on purpose — they all show the same desktop — but a preference belongs to the glass it is
   * set on, and sending it to the whole desk would change a robot nobody was looking at.
   */
  async setSettings(id: string, patch: Parameters<Surface['setSettings']>[0]): Promise<{ ok: boolean; error?: string }> {
    const entry = this.entries.get(id)
    if (!entry) return { ok: false, error: 'That device is not plugged into this computer.' }
    if (!entry.status.attached) return { ok: false, error: 'That device is unplugged.' }
    try {
      await entry.session.setSettings(patch)
      return { ok: true }
    } catch (error) {
      this.host.log(`cable: ${entry.label} settings: ${String(error)}`)
      return { ok: false, error: 'The device did not take the change.' }
    }
  }

  /**
   * The dial a WiFi change is for. Those messages go over the cable only, so this is a USB-attached dial:
   * the one named, or the only one there is.
   */
  private usbDial(id?: string): { entry: Entry } | { error: string } {
    const want = id?.trim().toUpperCase()
    if (want) {
      const entry = this.entries.get(want)
      if (!entry) return { error: `No dial ${id} is known to this computer.` }
      if (entry.via !== 'usb' || !entry.status.attached) return { error: `Dial ${id} is not plugged in over USB. WiFi is set over the cable only.` }
      return { entry }
    }
    const usb = [...this.entries.entries()].filter(([, e]) => e.via === 'usb' && e.status.attached)
    if (usb.length === 1) return { entry: usb[0][1] }
    if (!usb.length) return { error: 'No dial is plugged in over USB. WiFi is set over the cable only.' }
    return { error: `Several dials are plugged in (${usb.map(([key]) => key).join(', ')}). Say which with --device <serial>.` }
  }

  private async wifiCall(id: string | undefined, run: (session: Surface) => Promise<{ ok: boolean; error?: string }>,
                         what: string): Promise<{ ok: boolean; error?: string; id?: string }> {
    const found = this.usbDial(id)
    if ('error' in found) return { ok: false, error: found.error }
    try {
      const result = await run(found.entry.session)
      return { ...result, id: found.entry.status.id }
    } catch (error) {
      this.host.log(`cable: ${found.entry.label} ${what}: ${String(error)}`)
      return { ok: false, error: 'The device did not take the change.' }
    }
  }

  /** Give a USB-attached dial a WiFi network. The password passes through and is kept nowhere. */
  setWifi(id: string | undefined, ssid: string, psk: string) {
    return this.wifiCall(id, session => session.setWifi(ssid, psk), 'wifi set')
  }

  /** Erase a USB-attached dial's saved network. */
  forgetWifi(id?: string) {
    return this.wifiCall(id, session => session.forgetWifi(), 'wifi forget')
  }

  /** Every device's WiFi as it last said, whichever link it is on. */
  wifiStatus(): Array<Pick<DialStatus, 'id' | 'mac' | 'attached' | 'transport' | 'wifi'>> {
    return [...this.entries.values()].map(e => e.status).filter(s => s.attached || s.settings || s.wifi)
      .map(({ id, mac, attached, transport, wifi }) => ({ id, mac, attached, transport, wifi }))
  }

  private async send<K extends keyof Surface>(name: K, ...args: Surface[K] extends (...args: infer A) => unknown ? A : never): Promise<void> {
    await Promise.allSettled([...this.entries.values()].map(async entry => {
      try {
        const method = entry.session[name] as (...args: unknown[]) => Promise<void>
        await method.apply(entry.session, args)
      } catch (error) { this.host.log(`cable: ${entry.label} ${name}: ${String(error)}`) }
    }))
  }
  syncAgents(...args: Parameters<Surface['syncAgents']>) { return this.send('syncAgents', ...args) }
  syncSwarms(...args: Parameters<Surface['syncSwarms']>) { return this.send('syncSwarms', ...args) }
  syncMachines(...args: Parameters<Surface['syncMachines']>) { return this.send('syncMachines', ...args) }
  followApp(...args: Parameters<Surface['followApp']>) { return this.send('followApp', ...args) }
  replaceNotifications(...args: Parameters<Surface['replaceNotifications']>) { return this.send('replaceNotifications', ...args) }
  agentSeen(...args: Parameters<Surface['agentSeen']>) { return this.send('agentSeen', ...args) }
  question(...args: Parameters<Surface['question']>) { return this.send('question', ...args) }
  questionClose(...args: Parameters<Surface['questionClose']>) { return this.send('questionClose', ...args) }
  turnStarted(...args: Parameters<Surface['turnStarted']>) { return this.send('turnStarted', ...args) }
  turnDone(...args: Parameters<Surface['turnDone']>) { return this.send('turnDone', ...args) }
  summary(...args: Parameters<Surface['summary']>) { return this.send('summary', ...args) }
  turnError(...args: Parameters<Surface['turnError']>) { return this.send('turnError', ...args) }
}
