# Wi-Fi Provisioning in Session-Only Mode

This example demonstrates `NETWORK_PROV_MODE_SESSION_ONLY`, which keeps the
protocomm transport alive after provisioning so that the application can reuse
the secure session for its own purposes.

It also demonstrates session pause and resume, which stop and restart BLE
advertising without touching the session.

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

Once the device joins the network, the example calls
`network_prov_mgr_session_pause()`. This stops advertising, which removes its
continuous coexistence cost, while the session, the endpoints, the security
context and any live connection stay intact.

A press of the BOOT button toggles the session between paused and resumed, so
both transitions can be observed with a BLE scanner. Set the pin with
`CONFIG_EXAMPLE_BOOT_BUTTON_GPIO` if the board does not use GPIO 0. The button
is handled by the `espressif/button` component, which the component manager
downloads during the build.

The example never calls `network_prov_mgr_deinit()`. That would stop the
transport, and the BLE stack cannot be restarted without a reboot.

## Configure and build

```
idf.py set-target <chip>
idf.py menuconfig      # Example Configuration
idf.py build flash monitor
```

Use `NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT` with this mode, as the
example does. `FREE_BLE` releases the BLE memory, which a resumable session
still needs.

## Provision the device

```
python ../../tool/esp_prov/esp_prov.py --transport ble \
    --service_name PROV_XXXXXX --sec_ver 1 --pop abcd1234 \
    --ssid <SSID> --passphrase <PASSWORD>
```

Run the same command without the Wi-Fi arguments to open a session on a device
that is already provisioned. While the session is paused the tool does not find
the device; after a BOOT button press it does.

## Pausing while a client is connected

Pause does not drop a live connection. Protocomm restarts advertising from its
own disconnect handler, so the component re-asserts the pause when it sees
`PROTOCOMM_TRANSPORT_BLE_DISCONNECTED`. Connect with a BLE scanner, press BOOT
to pause while still connected, then disconnect: the device must stay off the
air. This works on both the NimBLE and the Bluedroid host.
