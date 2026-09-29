// Proves the desktop app's REAL updater accepts the fork's manifest. Not a unit test of the fork's
// scripts: it drives DesktopUpdater (desktop/lib/update/desktop_updater.dart) through its public API
// with a `metadataUrl` override, exactly what the shipped app does with its build-time define.
//
// This file lives under fork/ so the patch stack never touches desktop/test; the workflow copies it to
// desktop/test/ (untracked, on the runner only) because flutter test resolves `package:harness` from
// inside the desktop package. Run: fork/scripts/run_desktop_channel_test.sh
//
// Environment:
//   FORK_DESKTOP_MANIFEST_URL   required, an http(s) URL of the desktop manifest
//   FORK_STAGE=1                also download and stage the artifact for THIS host (macOS: the real zip,
//                               unpacked with ditto and version-checked by the updater itself; Linux:
//                               the AppImage), which is what applying an update would start from
//   FORK_EXPECT_VERSION         if set, the version the manifest must carry
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:harness/update/desktop_updater.dart';

void main() {
  final url = Platform.environment['FORK_DESKTOP_MANIFEST_URL'];
  final stage = Platform.environment['FORK_STAGE'] == '1';
  final expected = Platform.environment['FORK_EXPECT_VERSION'];
  if (url == null || url.isEmpty) {
    test('FORK_DESKTOP_MANIFEST_URL is set', () => fail('set FORK_DESKTOP_MANIFEST_URL'));
    return;
  }

  DesktopUpdater updater({required bool linux, required String arch}) => DesktopUpdater(
    metadataUrl: url,
    releaseMode: true, // flutter test is never a release build; checkOnce refuses otherwise
    isLinux: linux,
    architecture: arch,
  );

  // What a build one patch below upstream's latest looks like. Any version below the fork's works;
  // this one is what an install of upstream's 1.2.27 reports.
  const installed = '1.2.27';

  test('Apple Silicon Mac is offered the fork build', () async {
    final result = await updater(linux: false, arch: 'arm64').check(currentVersion: installed);
    expect(result.status, DesktopUpdateCheckStatus.available, reason: 'manifest rejected or not newer');
    final info = result.update!;
    expect(semverGt(info.version, installed), isTrue);
    if (expected != null && expected.isNotEmpty) expect(info.version, expected);
    expect(info.url, contains('/releases/download/'));
    expect(info.url, endsWith('Harness-macos-arm64.zip'));
    expect(info.sha256, hasLength(64));
    expect(info.size, greaterThan(0));
  });

  test('a fork install is up to date on its own version', () async {
    final first = await updater(linux: false, arch: 'arm64').check(currentVersion: installed);
    final result = await updater(linux: false, arch: 'arm64').check(currentVersion: first.update!.version);
    expect(result.status, DesktopUpdateCheckStatus.upToDate);
  });

  test('Intel Mac is offered nothing (the fork builds Apple Silicon only)', () async {
    final result = await updater(linux: false, arch: 'x64').check(currentVersion: installed);
    expect(result.status, isNot(DesktopUpdateCheckStatus.available));
  });

  for (final arch in ['x64', 'arm64']) {
    test('Linux $arch reads its own key when the fork published one', () async {
      final result = await updater(linux: true, arch: arch).check(currentVersion: installed);
      // A fork run that skipped an architecture has no entry: "failed" (no entry), never a wrong build.
      if (result.status == DesktopUpdateCheckStatus.available) {
        expect(result.update!.url, endsWith('Harness-linux-$arch.AppImage'));
      } else {
        expect(result.status, DesktopUpdateCheckStatus.failed);
      }
    });
  }

  if (stage) {
    test('the artifact for this host downloads, verifies and stages', () async {
      final linux = Platform.isLinux;
      final arch = Platform.version.contains('arm64') || Platform.version.contains('aarch64') ? 'arm64' : 'x64';
      final u = updater(linux: linux, arch: linux ? arch : 'arm64');
      final check = await u.check(currentVersion: installed);
      expect(check.status, DesktopUpdateCheckStatus.available);
      final staged = await u.downloadAndStage(check.update!);
      // null = size, sha256, ditto unpack, or the Info.plist version check failed (see debug output)
      expect(staged, isNotNull, reason: 'downloadAndStage rejected the fork artifact');
      expect(staged!.version, check.update!.version);
      expect(FileSystemEntity.typeSync(staged.bundlePath), isNot(FileSystemEntityType.notFound));
      final out = Platform.environment['FORK_STAGED_PATH_FILE'];
      if (out != null) File(out).writeAsStringSync(staged.bundlePath);
    }, timeout: const Timeout(Duration(minutes: 10)));
  }
}
