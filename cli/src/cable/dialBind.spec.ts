import { mkdtempSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'

import { bindTokenForTcp, bindTokenForUsb, DIAL_BIND_FILE, isTcpDialPath, pairedDialMacs, setDialBindDirForTest, unbindDial } from './dialBind.js'

describe('dial bind', () => {
  let dir: string
  beforeEach(() => { dir = mkdtempSync(join(tmpdir(), 'bind-')); setDialBindDirForTest(dir) })
  afterEach(() => setDialBindDirForTest(undefined))

  it('mints a token on USB and reuses it, and TCP cannot invent one', () => {
    expect(bindTokenForTcp('aa:bb:cc:dd:ee:ff')).toBeNull()
    const a = bindTokenForUsb('aa:bb:cc:dd:ee:ff')
    expect(a).toMatch(/^[0-9a-f]{64}$/)
    expect(bindTokenForUsb('AA:BB:CC:DD:EE:FF')).toBe(a)
    expect(bindTokenForTcp('aa:bb:cc:dd:ee:ff')).toBe(a)
    expect(bindTokenForTcp('11:22:33:44:55:66')).toBeNull()
    expect(bindTokenForUsb('')).toBe('')
    expect(isTcpDialPath('tcp:10.0.0.8:17420')).toBe(true)
    expect(isTcpDialPath('/dev/cu.usbmodem1101')).toBe(false)
  })

  it('persists in the documented shape, private to the user, one token per MAC', () => {
    const a = bindTokenForUsb('AA:BB:CC:DD:EE:01')
    const b = bindTokenForUsb('AA:BB:CC:DD:EE:02')
    expect(a).not.toBe(b)
    const file = join(dir, DIAL_BIND_FILE)
    expect(JSON.parse(readFileSync(file, 'utf8'))).toEqual({ tokens: { 'AA:BB:CC:DD:EE:01': a, 'AA:BB:CC:DD:EE:02': b } })
    expect(statSync(file).mode & 0o077).toBe(0)
    expect(pairedDialMacs().sort()).toEqual(['AA:BB:CC:DD:EE:01', 'AA:BB:CC:DD:EE:02'])
  })

  it('can forget a dial, and ignores a damaged file or a malformed token', () => {
    const a = bindTokenForUsb('AA:BB:CC:DD:EE:01')
    expect(unbindDial('aa:bb:cc:dd:ee:01')).toBe(true)
    expect(bindTokenForTcp('AA:BB:CC:DD:EE:01')).toBeNull()
    expect(unbindDial('AA:BB:CC:DD:EE:01')).toBe(false)
    writeFileSync(join(dir, DIAL_BIND_FILE), JSON.stringify({ tokens: { 'AA:BB:CC:DD:EE:03': 'short' } }))
    expect(bindTokenForTcp('AA:BB:CC:DD:EE:03')).toBeNull()
    writeFileSync(join(dir, DIAL_BIND_FILE), '{not json')
    expect(pairedDialMacs()).toEqual([])
    expect(bindTokenForUsb('AA:BB:CC:DD:EE:01')).not.toBe(a)
  })
})
