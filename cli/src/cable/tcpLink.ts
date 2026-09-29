// The dial's second transport: the same frames over TCP, found by mDNS `_harness-dial._tcp`.
// Connecting is not authorization. CableSession only welcomes a MAC this computer has USB-paired, and
// nothing here connects to a service whose TXT `mac` is not one it was asked for.
import { isIP, Socket } from 'node:net'
import Bonjour from 'bonjour-service'

export const DIAL_TCP_PORT = 17420
export const DIAL_MDNS_TYPE = 'harness-dial'
const CONNECT_TIMEOUT_MS = 5_000

export interface DialAddress {
  host: string
  port: number
}

/** What the LAN currently says about dials, by upper-case MAC. */
export interface DialBrowser {
  find(mac: string): DialAddress | undefined
  stop(): void
}

interface Advertised {
  name?: string
  host?: string
  port?: number
  addresses?: string[]
  txt?: Record<string, unknown>
}

const macOf = (service: Advertised): string => {
  const raw = service.txt?.mac
  return typeof raw === 'string' ? raw.trim().toUpperCase() : ''
}

/**
 * A long-lived browse for dials. Started only while some paired dial is out of reach, and stopped
 * as soon as none is, so a computer with no paired dial (or no network) never opens a multicast socket.
 * Throws if the socket cannot be made; the caller treats that as "no network right now".
 */
export function browseDials(onError: (why: string) => void = () => {}): DialBrowser {
  const seen = new Map<string, DialAddress>()
  const byName = new Map<string, string>()
  const bonjour = new Bonjour({}, (error: unknown) => onError(String(error)))
  const browser = bonjour.find({ type: DIAL_MDNS_TYPE, protocol: 'tcp' })
  browser.on('up', (service: Advertised) => {
    const mac = macOf(service)
    const host = service.addresses?.find(a => isIP(a) === 4) ?? service.addresses?.[0]
    if (!mac || !host) return
    seen.set(mac, { host, port: service.port || DIAL_TCP_PORT })
    if (service.name) byName.set(service.name, mac)
  })
  browser.on('down', (service: Advertised) => {
    const mac = macOf(service) || (service.name ? byName.get(service.name) : undefined)
    if (mac) seen.delete(mac)
    if (service.name) byName.delete(service.name)
  })
  return {
    find: mac => seen.get(mac.trim().toUpperCase()),
    stop: () => {
      seen.clear()
      try { browser.stop() } catch { /* already stopped */ }
      try { bonjour.destroy() } catch { /* already destroyed */ }
    },
  }
}

/** A `CablePort` over one TCP connection. Same bytes as SerialLink. */
export class TcpLink {
  readonly path: string
  private closed = false
  private closePromise: Promise<void> | null = null
  private tail: Promise<void> = Promise.resolve()

  private constructor(host: string, port: number, private readonly sock: Socket,
                      private readonly onClosed: (why: string) => void) {
    this.path = `tcp:${host}:${port}`
  }

  static open(host: string, port: number, onData: (chunk: Buffer) => void,
              onClosed: (why: string) => void): Promise<TcpLink> {
    return new Promise((resolve, reject) => {
      const sock = new Socket()
      const link = new TcpLink(host, port, sock, onClosed)
      sock.setNoDelay(true)
      sock.setTimeout(CONNECT_TIMEOUT_MS, () => sock.destroy(new Error('connect timed out')))
      sock.once('error', reject)
      sock.connect(port, host, () => {
        sock.removeListener('error', reject)
        sock.setTimeout(0)
        sock.on('data', chunk => { if (!link.closed) onData(Buffer.from(chunk)) })
        sock.on('error', err => { void link.close(err.message) })
        sock.on('close', () => { if (!link.closed) void link.close('end of stream') })
        resolve(link)
      })
    })
  }

  get isOpen(): boolean {
    return !this.closed
  }

  write(bytes: Uint8Array): Promise<void> {
    if (this.closed) return Promise.reject(new Error('port closed'))
    const next = this.tail.then(() => new Promise<void>((resolve, reject) => {
      if (this.closed) { reject(new Error('port closed')); return }
      this.sock.write(Buffer.from(bytes), err => (err ? reject(err) : resolve()))
    }))
    this.tail = next.catch(() => {})
    return next
  }

  close(why = 'closed'): Promise<void> {
    if (this.closePromise) return this.closePromise
    this.closed = true
    this.sock.destroy()
    this.closePromise = Promise.resolve().then(() => this.onClosed(why))
    return this.closePromise
  }
}
