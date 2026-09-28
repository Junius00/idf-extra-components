# Wi-Fi Provisioning in Session-Only Mode

This example demonstrates `NETWORK_PROV_MODE_SESSION_ONLY`, which keeps the
protocomm transport alive after provisioning so that the application can reuse
the secure session for its own purposes.

## How it works

The manager starts in `NETWORK_PROV_MODE_SESSION_ONLY`. In this mode only
`prov-session` and `proto-ver` are registered on the transport. The
provisioning endpoints (`prov-config`, `prov-scan`, `prov-ctrl`) and the
provisioning state machine are absent.

* **First boot**, the device is not provisioned. The example calls
  `network_prov_mgr_enable_provisioning()` before
  `network_prov_mgr_start_provisioning()` to opt in to the full provisioning
  flow for that boot.
* **Later boots**, the device is already provisioned. The example does not call
  `network_prov_mgr_enable_provisioning()`, so BLE starts for local control
  only, with the application's `custom-data` endpoint.

The example never calls `network_prov_mgr_deinit()`. That would stop the
transport, and the BLE stack cannot be restarted without a reboot.

## Configure and build

```
idf.py set-target <chip>
idf.py menuconfig      # Example Configuration
idf.py build flash monitor
```

Use `NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT` with this mode, as the
example does. `FREE_BLE` releases the BLE memory, which the kept alive session
still needs.

## Provision the device

```
python ../../tool/esp_prov/esp_prov.py --transport ble \
    --service_name PROV_XXXXXX --sec_ver 1 --pop abcd1234 \
    --ssid <SSID> --passphrase <PASSWORD>
```

Run the same command without the Wi-Fi arguments to open a session on a device
that is already provisioned.
