#!/usr/bin/env python3
"""M-VAVE's firmware checks (sdk/Build.ps1, Test-Firmware), ported to run on the
Linux build: the linked image must keep the SDK's runtime tables, required
runtime and Bluetooth/USB symbols, the application's own callbacks (not weak
defaults), the fixed RTOS ABI, no unsupported runtime, the Bluetooth ACL pools,
the heap, every audio callback and the panel scanner in internal SRAM, the ROM
entry point and only the allocated sections the FM1 packaging covers.

Differences from Test-Firmware, each for a stated reason:
  - the engine library may be our copy (libfm1_engine_viscera.a, tools/strip_engines.py)
  - symbols reached only through the stock engines' render functions, which that
    copy leaves out, may be absent: EXPECTED_ABSENT below

  python3 tools/check_firmware.py ELF MAP SYNTH_JSON --toolchain BIN
"""
import argparse, json, os, re, struct, subprocess, sys

SRAM = (0x01c00000, 0x01c80000)
# gone with the stock engines' render functions (tools/strip_engines.py): only those used them
EXPECTED_ABSENT = {'bass303_render', 'bass303_advance_idle', 'fm1_sea_decode_groups'}

def fail(msg):
    raise SystemExit('firmware check failed: ' + msg)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf'); ap.add_argument('map'); ap.add_argument('synth')
    ap.add_argument('--toolchain', required=True)
    a = ap.parse_args()
    synth = json.load(open(a.synth))
    out = subprocess.run([os.path.join(a.toolchain, 'nm'), '--print-size', a.elf], capture_output=True, text=True).stdout
    info, heap, usb_midi = {}, {}, False
    for line in out.splitlines():
        m = re.match(r'^([0-9a-fA-F]+)[ \t]+(([0-9a-fA-F]+)[ \t]+)?([A-Za-z])[ \t]+(.+)$', line)
        if not m:
            continue
        name, address, size, kind = m.group(5), int(m.group(1), 16), int(m.group(3), 16) if m.group(3) else None, m.group(4)
        info[name] = (address, size, kind)
        if re.match(r'^(__fat_|fat_sdfile_fat_ops$|jl_fat_vfs_ops$|nor_sdfile_vfs_ops$|sdfile_ext_vfs_ops$)', name):
            fail('unused filesystem entered the fixed Flash runtime: ' + name)
        if re.match(r'^lv_', name):
            fail('unexpected LVGL runtime: ' + name)
        if re.search(r'usb.*midi|midi.*usb', name):
            usb_midi = True
    have = lambda n: n in info
    regs = [('initcall', 4, 4), ('early_initcall', 16, 4), ('late_initcall', 4, 4), ('platform_initcall', 4, 4),
            ('module_initcall', 4, 4), ('sys_event_handler', 12, 12), ('syscfg_ops', 84, 28), ('lp_target', 72, 8),
            ('sys_cpu_timer', 16, 16), ('clock_critical_handler', 16, 8), ('vfs_ops', 120, 120),
            ('audio_decoder', 56, 56), ('audio_hwaccel', 32, 32)]
    for name, least, step in regs:
        b, e = f'_{name}_begin', f'_{name}_end'
        if not (have(b) and have(e)):
            b, e = f'{name}_begin', f'{name}_end'
        if not (have(b) and have(e)):
            fail('runtime registration range missing: ' + name)
        size = info[e][0] - info[b][0]
        if size < least or size % step:
            fail('runtime registration table invalid: ' + name)
        if name == 'vfs_ops' and size != 120:
            fail('expected only the internal Flash filesystem registration')
    required = ['fm1_r_7919135c9963', 'fm1_r_1f449c2c7b8f', 'vSoftwareInterruptISR', 'fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3',
                'fm1_r_05431712fb7b', 'fm1_update_init', 'fm1_update_message', 'fm1_update_busy', 'fm1_build_id', 'fm1_flash_init',
                'sdfile_vfs_ops', 'sdfile_init', 'syscfg_vm_init', 'syscfg_btif_init', 'wifi_get_xosc', 'wifi_get_mcs_dgain',
                'wifi_get_pa_trim_data', 'connectivity_wl82_bluetooth_start', 'ble_profile_init', 'att_server_init',
                'fm1_ble_set_role', 'fm1_ble_scan', 'fm1_ble_connect', 'fm1_ble_disconnect', 'fm1_ble_get_status', 'fm1_ble_get_device',
                'audio_app_usb_microphone_enable', 'audio_app_usb_speaker_enable', 'connectivity_audio_usb_microphone_enable',
                'connectivity_audio_usb_speaker_enable', 'audio_app_bluetooth_open', 'sbc_decoder', 'sbc_hwaccel',
                'avdtp_packet_handler', 'bt_get_local_name', 'bt_get_mac_addr', 'fm1_bluetooth_configure',
                'fm1_bluetooth_get_status', 'groove_tuning_capture', 'groove_tuning_apply', 'groove_project_apply_edit']
    for n in required:
        if not have(n):
            fail('required runtime symbol absent: ' + n)
    for n in ('sdp_send_cmd_iotl', 'sdp_master_packet_handler', 'gatt_client_discover_primary_services_by_uuid128'):
        if not have(n) or info[n][2] not in 'TtNn' or (info[n][1] or 0) <= 16:
            fail('Bluetooth discovery implementation missing or stubbed: ' + n)
    for n in ('fm1_r_dee1ea411566', 'fm1_r_d16405f329ed', 'fm1_r_f03cc16d330d', 'fm1_r_1993f9b103e3', 'fm1_io_trs_midi_enabled',
              'fm1_r_05431712fb7b', 'fm1_r_0aa010fe1de1', 'fm1_r_33d5e5b63b6b', 'fm1_app_sysex'):
        if not have(n) or info[n][2] not in 'TNtn' or not info[n][1]:
            fail('application callback was lost or replaced by a weak default: ' + n)
    if info['fm1_build_id'][1] != 32:
        fail('browser update build identity must be exactly 32 bytes')
    if not have('fm1_update_identity') or info['fm1_update_identity'][1] != 27:
        fail('FM1 update identity must be exactly 27 bytes')
    if not usb_midi:
        fail('USB MIDI runtime is missing')
    for n in ('CurrentTCB', 'highCurrentTCB', 'xIdleTaskHandle'):
        if not have(n) or info[n][1] != 4:
            fail('platform RTOS ABI is invalid: ' + n)
    for n in ('__rt_local_irq_disable', '__rt_local_irq_enable', 'bt_lock_cnt', 'fm1_r_83b2de623a96', 'fm1_r_c327ea18da09', 'cpuSuspend',
              'audio_app_worker', 'edr_name', 'ble_name', 'a2dp_source_event_handler', 'a2dp_source_sbc_codec', 'hfp_profile_init',
              'hfp_ag_profile_init', 'spp_user_send_data', 'avrcp_player_event', 'hid_diy_regiest_callback', 'obex_connection_req',
              'pan_cmd_io_ctrl', 'gatt_client_discover_primary_services', 'ancs_client_init', 'ams_client_init', 'sdio_host_init',
              'wl30_sdio_claim_irq', 'wl30_sdio_release_irq', 'wifi_on', 'wifi_off', 'wifi_rx_task', 'wifi_tx_task',
              'ui_yuv420_to_rgb565_pairs', 'ui_tiny_ttf_pack_scanline'):
        if have(n):
            fail('unsupported platform runtime entered FM1: ' + n)
    mp = open(a.map, errors='replace').read()
    if 'libfm1_engine.a(' not in mp and 'libfm1_engine_viscera.a(' not in mp:
        fail('fixed native SDK archive missing')
    if 'libfm1_runtime.a(' in mp:
        fail('only the fixed music/BLE SDK may be linked')
    for i in ('/system.a(', 'audio_server.a(', 'media_app.a(', 'btstack.a(', 'lib_ui.a(', 'libjpeg_decode_only.a('):
        if i in mp:
            fail('unsupported FM1 link input: ' + i)
    for m in ('a2dp_source', 'ams_client', 'ancs_client', 'avctp', 'avrcp', 'bip', 'bnep', 'bt_audio', 'client_user', 'hfp_ag_profile',
              'hfp_profile', 'hid', 'iap_profile', 'obex', 'opp', 'pan', 'pbg_profile', 'rfcomm', 'spp_profile', 'spp_update_profile',
              'spp_user', 'vCard'):
        if f'.a({m}.c.o)' in mp:
            fail('unsupported FM1 link member: ' + m)
    acl = {}
    for d in ('tx', 'rx'):
        b, e = f'acl_{d}_pool', f'acl_{d}_pool_end'
        if not (have(b) and have(e)):
            fail('required controller pool missing: ' + b)
        acl[d] = info[e][0] - info[b][0]
    if acl['tx'] < 1024 or acl['rx'] < 4096:
        fail('classic ACL pools too small; need 1024/4096 bytes')
    m = re.search(r'0x([0-9a-fA-F]+)[^\r\n]*_MALLOC_SIZE[ ]*=', mp)
    if not m:
        fail('cannot find runtime heap in link map')
    heap_bytes = int(m.group(1), 16)
    if heap_bytes < 112640:            # (M-VAVE's script: 49152; see MIN_HEAP in build_fm1.py)
        fail(f'FM1 runtime heap is {heap_bytes} bytes; need at least 112640 (USB, Bluetooth and MIDI start-up)')
    callbacks = ['fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3', 'fm1_r_05431712fb7b', 'fm1_r_ffe3f82683f6', 'fm1_audio_irq',
                 'fm1_flash_worker_prepare', 'fm1_r_7e12ed19990d', 'fm1_r_74310ca1fed9', 'fm1_engine_render', 'fm1_engine_output_level',
                 'fm1_audio_copy', 'fm1_output_mix', 'fm1_usb_audio_read', 'fm1_bluetooth_audio_read', 'fm1_usb_audio_write',
                 'connectivity_audio_get_speaker_volume', 'connectivity_audio_get_mic_volume']
    callbacks += ['groove_sequence_render', 'groove_sequence_clock', 'groove_sequence_position', 'groove_sequence_transport',
                  'fm1_sea_decode_groups', 'fm1_codec_sea_mono_s16_run'] + list(synth['sram_required']) + [
        'synth_dx7_render', 'synth_dx7_midi', 'synth_dx7_key', 'fm1_dx7_panic', 'fm_exp2_fixed', 'bass303_bank_render', 'bass303_bank_key',
        'bass303_bank_set', 'drum808_kit_render', 'drum808_kit_trigger', 'psg_render', 'psg_key', 'psg_midi', 'psg_panic', 'psg_set',
        'psg_get', 'pluck_render', 'pluck_key', 'pluck_midi', 'pluck_panic', 'pluck_set', 'pluck_get', 'fm1_mix128',
        'fm1_mix_mono_stereo128', 'fm1_bus_output128', 'fm1_master_mix128', 'fm1_master_gain', 'fm1_declick128', 'fm1_render_begin',
        'fm1_render_end', 'synth_dx7_active', 'bass303_bank_active', 'drum808_kit_active', 'psg_active', 'pluck_active',
        'sampledrum_active', 'synth_dx7_limit', 'psg_limit', 'pluck_limit', 'sampledrum_limit', 'bass303_advance_idle', 'fm1_send128',
        'fm1_fx_render', 'fm1_fx_clear', 'fm1_fx_active', 'sampledrum_render', 'sampledrum_trigger', 'sampledrum_panic', 'psg_active_chips']
    for n in list(synth['sram_optional']) + list(synth['native_sram_optional']):
        if have(n): callbacks.append(n)
    callbacks += ['cpu_enter_sfc_critical', 'cpu_exit_sfc_critical', 'enter_spi_code', 'exit_spi_code', 'fm1_r_da3931a5b7a8', 'sfc_drop_cache']
    for n in ('audio_app_usb_mix', 'audio_app_usb_record', 'audio_app_usb_replace_input', 'render_stereo', 'effects_render', 'effects_sample',
              'sequence_event', 'clock_publish', 'groove_arp_event', 'groove_arp_running', 'engine_note'):
        if have(n): callbacks.append(n)
    if have('fm1_r_0aa010fe1de1') and info['fm1_r_0aa010fe1de1'][0] > 0:
        callbacks.append('fm1_r_0aa010fe1de1')
        if have('fm1_r_33d5e5b63b6b') and info['fm1_r_33d5e5b63b6b'][0] > 0:
            callbacks.append('fm1_r_33d5e5b63b6b')
    absent = set()
    for n in callbacks:
        if not have(n):
            if n in EXPECTED_ABSENT:
                absent.add(n); continue
            fail('audio callback absent: ' + n)
        if not (SRAM[0] <= info[n][0] < SRAM[1]):
            fail('audio callback is outside internal SRAM: ' + n)
    for n in ('fm1_input_tick', 'fm1_panel_irq', 'fm1_scan_decode', 'fm1_key_pin', 'fm1_encoder_pin', 'fm1_quadrature'):
        if not have(n) or not (SRAM[0] <= info[n][0] < SRAM[1]):
            fail('panel scan code or wiring table is outside internal SRAM: ' + n)
    raw = open(a.elf, 'rb').read()
    if raw[:6] != b'\x7fELF\x01\x01':
        fail('expected ELF32 little endian')
    entry, = struct.unpack_from('<I', raw, 24)
    if entry != 0x02000120 or not have('_start') or info['_start'][0] != entry or not have('_text_rodata_begin') \
            or info['_text_rodata_begin'][0] != entry:
        fail('firmware startup must match the fixed ROM entry and the first packaged instruction')
    shoff, = struct.unpack_from('<I', raw, 32)
    stride, count, stri = struct.unpack_from('<HHH', raw, 46)
    so, ss = struct.unpack_from('<II', raw, shoff + stri * stride + 16)
    app = 0
    for i in range(count):
        h = shoff + i * stride
        nameoff, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', raw, h)
        if not (flags & 2) or not size:
            continue
        name = raw[so + nameoff:raw.index(0, so + nameoff)].decode()
        if not re.match(r'^\.(text|data|bss|ram0_data|ram0_bss|boot_info|cache_ram_data|cache_ram_bss)$', name):
            fail('unsupported allocated section: ' + name)
        if typ != 8:
            if re.match(r'^\.(text|data|ram0_data|cache_ram_data)$', name): app += size
            elif name != '.boot_info': fail('allocated section is not covered by FM1 packaging: ' + name)
    print(f"M-VAVE's firmware checks pass: app image {app} bytes, heap {heap_bytes}"
          + (f"; absent with the stock render functions: {', '.join(sorted(absent))}" if absent else ''))

if __name__ == '__main__':
    main()
