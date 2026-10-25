
# Default LCD top line display messages and RGB LED meanings:

| Top Line             | RGB LED         | Meaning                                                            |
| -------------------- | --------------- | ------------------------------------------------------------------ |
| ESP32 Time Server    | Blue            | The server is in startup mode                                      |
| ESP32 Time Server    | Blue flashing   | The server is almost ready, but needs to complete PPS disciplining |
|                      |                 |                                                                    |
| ESP32 Time Server    | Green           | The server is operating normally                                   |
| ESP32 Time Server[3] | Yellow flashing | There are queued MQTT items                                        |
| ESP32 Time Server[H] | Yellow flashing | PPS holdover: PPS lost, still stratum 1 for the holdover time      |
| ESP32 Time Server[5] | Yellow          | PPS missing                                                        |
| ESP32 Time Server[6] | Yellow          | GNSS missing or invalid                                            |
| ESP32 Time Server[7] | Yellow          | GNSS sync stale                                                    |
| ESP32 Time Server[8] | Yellow          | GNSS has lost its satellite lock                                   |
| ESP32 Time Server[M] | Red flashing    | Not connected to the MQTT broker                                   |
| ESP32 Time Server[9] | Red             | Communication failure with the GNSS receiver                       |
| ESP32 Time Server[1] | Red             | Ethernet not connected                                             |
| ESP32 Time Server[2] | Red             | MQTT setup failed                                                  |
| ESP32 Time Server[T] | Red             | Transport layer stalled                                            |
| ESP32 Time Server[4] | Red             | Sanity check mismatch                                              |
|                      |                 |                                                                    |
| ESP32 Time Server[I] | Orange          | No usable IP address                                               |
| ESP32 Time Server[+] | Orange flashing | Load shedding active                                               |
| ESP32 Time Server[*] | White           | The server is setting its time                                     |
| ESP32 Time Server's  | Pink            | The button is being pressed                                        |


>Note:
>In timekeeping terminology, holdover is the state where a time server continues providing time from its internal
>oscillator after losing its external reference (in this case the GNSS). The device is no longer actively
>synchronized but is "coasting" on its last-known good time.  The PPS holdover (stratum 1 for a short time after the PPS is lost, see the GNSS and PPS
>settings of the web admin page) is shown as [H]; after it the PPS fault [5] is shown.


