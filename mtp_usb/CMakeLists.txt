# Carrier of the USB files-mode dependency (Docs/MTP/Merge_Report.md MTP-M-2).
# This component has no source. Its manifest (idf_component.yml) pulls the forked esp_tinyusb. The top CMakeLists.txt
# adds this folder to the build ONLY for the env daq_mtp (-DDAQ_MTP_USB=ON). The env daq never sees it.
idf_component_register(INCLUDE_DIRS ".")
