Signed PawnIO modules used by Hoverdock for native CPU package temperature.

IntelMSR.bin
  IA32_PACKAGE_THERM_STATUS / IA32_TEMPERATURE_TARGET (GenuineIntel).
  Origin: LibreHardwareMonitorLib embedded resource (kept from v1.1.63).

AMDFamily17.bin
  ioctl_read_smn for Zen family 17h/19h/1Ah THM_TCON_CUR_TMP (AuthenticAMD).
  Origin: namazso PawnIO.Modules release 0.2.11
    https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip

License: GNU Lesser General Public License v2.1 (see COPYING).
Source for the module scripts: https://github.com/namazso/PawnIO.Modules

Temperature formulas follow LibreHardwareMonitor (MPL-2.0):
  https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
