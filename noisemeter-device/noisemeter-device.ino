/// @file
/// @brief Main firmware code
/* noisemeter-device - Firmware for CivicTechTO's Noisemeter Device
 * Copyright (C) 2024  Clyne Sullivan, Nick Barnard
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

#include "access-point.h"
#include "api.h"
#include "blinker.h"
#include "board.h"
#include "data-packet.h"
#include "spl-meter.h"
#include "storage.h"
#include "ota-update.h"
#include "UUID/UUID.h"

#include <cstdint>
#include <list>
#include <optional>

#if defined(BUILD_PLATFORMIO) && defined(BOARD_ESP32_PCB)
HWCDC USBSerial;
#endif

// Uncomment these to disable WiFi and/or data upload
//#define UPLOAD_DISABLED

/** Maximum number of milliseconds to wait for successful WiFi connection. */
constexpr auto WIFI_CONNECT_TIMEOUT_MS = 20 * 1000;
/** Specifies how frequently to upload data points to the server. */
const unsigned long UPLOAD_INTERVAL_SEC = 60 * 5; // Upload every 5 mins
/** Specifies how frequently to check for OTA updates from our server. */
const unsigned long OTA_INTERVAL_SEC = 60 * 60 * 24; // Check for updates daily
/** Maximum number of data packets to retain when WiFi is unavailable. */
constexpr unsigned MAX_SAVED_PACKETS = 14 * 24 * 60 * 60 / UPLOAD_INTERVAL_SEC;

/** SPLMeter instance to manage decibel level measurement. */
static SPLMeter SPL;
/** Storage instance to manage stored credentials. */
static Storage Creds;
/** Linked list of completed data packets.
 * This list should only grow if WiFi is unavailable. */
static std::list<DataPacket> packets;
/** Tracks when the last measurement upload occurred. */
static Timestamp lastUpload = Timestamp::invalidTimestamp();
/** Tracks when the last OTA update check occurred. */
static Timestamp lastOTACheck = Timestamp::invalidTimestamp();

/**
 * Outputs the given decibel reading over serial.
 * @param reading The decibel reading to display
 */
void printReadingToConsole(double reading);

/**
 * Callback for AccessPoint that verifies credentials and attempts registration.
 * @param httpServer HTTP server which served the setup form
 * @return An error message if not successful
 */
std::optional<const char *> saveNetworkCreds(WebServer& httpServer);

/**
 * Generates a UUID that is unique to the hardware running this firmware.
 * @return A device-unique UUID object
 */
UUID buildDeviceId();

/**
 * Attempt to establish a WiFi connected using the stored credentials.
 * @param mode WiFi mode to run in (e.g. WIFI_STA or WIFI_AP_STA)
 * @param timout Connection timeout in milliseconds
 * @return Zero on success or a negative number on failure
 */
int tryWifiConnection(wifi_mode_t mode = WIFI_STA, int timeout = WIFI_CONNECT_TIMEOUT_MS);

/**
 * Firmware entry point and initialization routine.
 */
void setup() {
  pinMode(PIN_LED1, OUTPUT);
  digitalWrite(PIN_LED1, LOW);

  // Grounding this pin (e.g. with a button) will force access open to start.
  // Useful as a "reset" button to overwrite currently saved credentials.
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  SERIAL.begin(115200);
  delay(2000);
  SERIAL.println();
  SERIAL.print("Noisemeter ");
  SERIAL.print(NOISEMETER_VERSION);
  SERIAL.print(' ');
  SERIAL.println(buildDeviceId());
  SERIAL.println("Initializing...");

  Creds.begin(buildDeviceId());
#ifdef STORAGE_SHOW_CREDENTIALS
  SERIAL.println(Creds);
#endif

  SPL.initMicrophone();
  packets.emplace_front();

#ifndef UPLOAD_DISABLED
  bool isAPNeeded = false;

  if (!digitalRead(PIN_BUTTON) || !Creds.valid()) {
    SERIAL.print("Erasing stored credentials...");
    Creds.clear();
    SERIAL.println(" done.");

    isAPNeeded = true;
  } else if (Creds.get(Storage::Entry::Token).length() == 0) {
    isAPNeeded = true;
  } else if (tryWifiConnection() < 0 || Timestamp::synchronize() < 0) {
    isAPNeeded = true;
  }

  // Run the access point if it is requested or if there are no valid credentials.
  if (isAPNeeded) {
    AccessPoint ap (saveNetworkCreds);
    Blinker bl (500);

    ap.run(); // does not return
  }

  Timestamp now;
  lastUpload = now;
  lastOTACheck = now;

  SERIAL.println("Connected to the WiFi network.");
  SERIAL.print("Local ESP32 IP: ");
  SERIAL.println(WiFi.localIP());
  SERIAL.print("Current time: ");
  SERIAL.println(now);
#endif // !UPLOAD_DISABLED

  digitalWrite(PIN_LED1, HIGH);
}

/**
 * Main loop for the firmware.
 * This function is run continuously, getting called within an infinite loop.
 */
void loop() {
  if (auto db = SPL.readMicrophoneData(); db) {
    packets.front().add(*db);
    printReadingToConsole(*db);
  }

#ifndef UPLOAD_DISABLED
  const auto now = Timestamp();

  if (lastUpload.secondsBetween(now) >= UPLOAD_INTERVAL_SEC) {
    packets.front().timestamp = now;

    if (WiFi.status() != WL_CONNECTED) {
      SERIAL.println("Attempting WiFi reconnect...");
      WiFi.reconnect();
      delay(5000);
    }

    if (WiFi.status() == WL_CONNECTED) {
      API api (buildDeviceId(), Creds.get(Storage::Entry::Token));

      {
        std::optional<Blinker> bl;

        // Only blink if there's multiple packets to send
        if (++packets.cbegin() != packets.cend())
          bl.emplace(300);

        packets.remove_if([&api](const auto& pkt) {
          return pkt.count <= 0 || api.sendMeasurement(pkt);
        });
      }

#if defined(BOARD_ESP32_PCB)
      // We have WiFi: also check for software updates
      if (lastOTACheck.secondsBetween(now) >= OTA_INTERVAL_SEC) {
        lastOTACheck = now;
        SERIAL.println("Checking for updates...");

        const auto ota = api.getLatestSoftware();
        if (ota) {
          if (ota->version.compareTo(NOISEMETER_VERSION) > 0) {
            SERIAL.print(ota->version);
            SERIAL.println(" available!");

            if (downloadOTAUpdate(ota->url, api.rootCertificate())) {
              SERIAL.println("Download success! Restarting...");
              delay(1000);
              ESP.restart();
            } else {
              SERIAL.println("Update download failed.");
            }
          } else {
            SERIAL.println("No new updates.");
          }
        } else {
          SERIAL.println("Failed to reach update server!");
        }
      }
#endif // BOARD_ESP32_PCB
    }

    if (!packets.empty()) {
      const auto count = std::distance(packets.cbegin(), packets.cend());
      SERIAL.print(count);
      SERIAL.println(" packets still need to be sent!");

      if (count >= MAX_SAVED_PACKETS) {
        SERIAL.println("Discarded a packet!");
        packets.pop_back();
      }
    }

    // Create new packet for next measurements
    packets.emplace_front();
    lastUpload = now;
  }
#endif // !UPLOAD_DISABLED
}

void printReadingToConsole(double reading) {
  String output = "";
  output += std::lround(reading);
  output += "dB";

  const auto currentCount = packets.front().count;
  if (currentCount > 1) {
    output += " [+" + String(currentCount - 1) + " more]";
  }
  SERIAL.print("\r                    \r");
  SERIAL.print(output);
}

std::optional<const char *> saveNetworkCreds(WebServer& httpServer)
{
  // Confirm that the form was actually submitted.
  if (httpServer.hasArg("ssid") && httpServer.hasArg("psk")) {
    const auto ssid = httpServer.arg("ssid");
    const auto psk = httpServer.arg("psk");
    const auto email = httpServer.arg("email");

    // Confirm that the given credentials will fit in the allocated EEPROM space.
    if (!ssid.isEmpty() && Creds.canStore(ssid) && Creds.canStore(psk) && Creds.canStore(email)) {
      Creds.set(Storage::Entry::SSID, ssid);
      Creds.set(Storage::Entry::Passkey, psk);
      Creds.set(Storage::Entry::Token, {});
      Creds.commit();

      if (tryWifiConnection(WIFI_AP_STA) == 0 && Timestamp::synchronize() == 0) {
        API api (buildDeviceId());
        const auto registration = api.sendRegister(email);

        if (registration) {
          SERIAL.println("Registered!");
          Creds.set(Storage::Entry::Token, *registration);
          Creds.commit();

          return {};
        } else {
          return "Device registration failed!";
        }
      } else {
        return "Failed to connect to the internet!";
      }
    }
  }

  return "Invalid network credentials!";
}

UUID buildDeviceId()
{
  std::array<uint8_t, 6> mac;
  esp_efuse_read_field_blob(ESP_EFUSE_MAC_FACTORY, mac.data(), /* bits */ mac.size() * 8);
  return UUID(mac[0] | (mac[1] << 8) | (mac[2] << 16), mac[3] | (mac[4] << 8) | (mac[5] << 16));
}

int tryWifiConnection(wifi_mode_t mode, int timeout)
{
  WiFi.mode(mode);
  const auto stat = WiFi.begin(
    Creds.get(Storage::Entry::SSID).c_str(),
    Creds.get(Storage::Entry::Passkey).c_str());
  if (stat == WL_CONNECT_FAILED)
    return -1;

  // wait for WiFi connection
  SERIAL.print("Waiting for WiFi to connect...");
  const auto start = millis();
  bool connected;

  do {
    connected = WiFi.status() == WL_CONNECTED;
    SERIAL.print(".");
    delay(500);
  } while (!connected && millis() - start < timeout);

  return connected ? 0 : -1;
}

