# PostureSense – Smart Posture & Screen Distance Monitoring System

**An ESP32-Based IoT System for Real-Time Posture Monitoring, Ergonomic Alerts, and Cloud-Based Analytics**

PostureSense is a low-cost, privacy-friendly IoT prototype designed to help office workers and students maintain healthier sitting habits. It monitors forward neck lean, spine bending, and the distance between the user and their computer screen. When an unhealthy posture or screen distance is detected, the system provides immediate local alerts and records monitoring data in Firebase Realtime Database.

The system also includes a web dashboard for real-time monitoring, historical data visualization, remote configuration, and Telegram notifications.

## 📌 Project Overview

Prolonged desk work can lead to poor sitting habits, including forward head posture, rounded backs, and sitting too close to computer screens. These habits may contribute to neck discomfort and digital eye strain.

PostureSense addresses this problem using two independent ESP32-based IoT nodes:

- **Wearable Posture Monitoring Node:** Measures forward neck lean and spine bending using an MPU6050 accelerometer and a flex sensor.
- **Screen Distance Monitoring Node:** Uses an HC-SR04 ultrasonic sensor to measure the user's distance from the screen.

Both nodes process sensor readings locally, generate alerts when predefined thresholds are exceeded, and upload monitoring data to Firebase over Wi-Fi.

## ✨ Key Features

- **Real-Time Posture Monitoring:** Tracks forward neck lean and spine bending.
- **Screen Distance Detection:** Detects when the user sits too close to the screen.
- **Instant Local Alerts:** Uses a buzzer and vibration motor to notify the wearer of poor posture.
- **Visual and Audible Feedback:** Provides LED and buzzer alerts from the screen-distance monitoring node.
- **Cloud Data Storage:** Stores live readings, historical measurements, and alert events in Firebase Realtime Database.
- **Interactive Web Dashboard:** Displays live sensor values, system status, historical charts, and recent alerts.
- **Remote Configuration:** Allows users to adjust wearable posture thresholds, alert hold time, session duration, and feedback settings.
- **Telegram Notifications:** Sends messages when posture or session-limit alerts start.
- **Sensor Calibration:** Uses a guided calibration process to establish the user's upright posture.
- **Noise Filtering:** Applies median filtering, low-pass filtering, and hysteresis to reduce unstable readings and false alerts.
- **Fault Recovery:** Attempts to recover from MPU6050 communication failures and Wi-Fi disconnections.
- **Privacy-Friendly Design:** Uses physical sensors rather than cameras or microphones.

## 🏗️ System Architecture

PostureSense consists of two independent sensing nodes, a cloud database, a web dashboard, and a Telegram notification channel.

```text
                  ┌────────────────────────────┐
                  │   Wearable ESP32 Node      │
                  │                            │
                  │  MPU6050 Accelerometer     │
                  │  Flex Sensor               │
                  │  Buzzer + Vibration Motor  │
                  └─────────────┬──────────────┘
                                │
                                │ Wi-Fi / HTTPS REST
                                ▼
                  ┌────────────────────────────┐
                  │  Firebase Realtime Database│
                  └─────────────┬──────────────┘
                                │
                 ┌──────────────┴──────────────┐
                 │                             │
                 ▼                             ▼
       ┌────────────────────┐       ┌────────────────────┐
       │   Web Dashboard    │       │ Telegram Bot API   │
       │                    │       │                    │
       │ Live Monitoring    │       │ Alert Notifications│
       │ History Charts     │       └────────────────────┘
       │ Remote Settings    │
       └────────────────────┘

                  ┌────────────────────────────┐
                  │  Screen Monitor ESP32      │
                  │                            │
                  │ HC-SR04 Ultrasonic Sensor  │
                  │ Buzzer + LED               │
                  └─────────────┬──────────────┘
                                │
                                └── Wi-Fi / HTTPS REST
                                    to Firebase
```

Each node operates independently and writes to its own database paths. Local sensing and alerting do not depend on a successful cloud connection.

## 🔧 Hardware Components

| Component | Quantity | Purpose |
|---|---:|---|
| ESP32 development board | 2 | Main controllers and Wi-Fi communication |
| MPU6050 accelerometer module | 1 | Measures neck lean |
| Flex sensor | 1 | Estimates spine bending |
| HC-SR04 ultrasonic sensor | 1 | Measures screen distance |
| Buzzer | 2 | Audible alerts |
| Vibration motor | 1 | Wearable tactile feedback |
| Push button | 1 | Calibration and session control |
| LED | 1 | Screen-distance status indication |
| Resistors, driver components, wires, strap, and enclosure | As required | Circuit assembly and mounting |

**Note:** Use appropriate driver circuitry for the vibration motor and any buzzer that exceeds the ESP32 GPIO drive capability. The HC-SR04 echo signal must also be reduced to a safe ESP32 input voltage.

## 💻 Software and Technologies

- **Programming Language:** C/C++ using the Arduino framework
- **Microcontroller Platform:** ESP32
- **Development Environment:** Arduino IDE
- **Cloud Database:** Firebase Realtime Database
- **Communication:** Wi-Fi and HTTPS REST API
- **JSON Processing:** ArduinoJson 7
- **Concurrent Tasks:** FreeRTOS
- **Web Technologies:** HTML, CSS, and JavaScript
- **Data Visualization:** Chart.js 4.4.1
- **Notifications:** Telegram Bot API
- **Simulation:** Wokwi for supported monitor-node testing

## 📐 Sensor Monitoring and Alert Logic

### 1. Neck Lean Detection

The MPU6050 measures acceleration along three axes. During calibration, the wearable learns the gravity direction in the user's upright position and the direction associated with a guided forward lean.

The firmware calculates a signed neck-lean angle using a gravity-vector method, allowing the sensor to be mounted in different orientations.

Default neck-lean threshold: **15 degrees**

### 2. Spine Bending Detection

The flex sensor changes resistance as it bends. A voltage divider converts this change into an analog voltage that the ESP32 reads.

The firmware takes 15 ADC samples and uses their median to reduce sudden measurement fluctuations. An exponential filter smooths the result, which is expressed as a percentage change from the upright baseline.

Default spine-bend threshold: **12%**

### 3. Screen Distance Monitoring

The HC-SR04 ultrasonic sensor estimates the distance between the user and the screen.

| Condition | Behaviour |
|---|---|
| Distance below 50 cm | User is considered too close |
| Distance above 55 cm | Too-close state is cleared |
| Distance beyond 120 cm | Status changes to `NO ONE` |
| Too close continuously for 10 seconds | Status changes to `ALERT` |

The 50–55 cm hysteresis range helps prevent repeated state changes when measurements fluctuate around the threshold.

### 4. Local Alerts

The wearable triggers an alert when poor posture continues beyond the configured hold time.

Default settings:

| Setting | Default |
|---|---:|
| Neck-lean limit | 15 degrees |
| Spine-bend limit | 12% |
| Posture alert hold time | 7 seconds |
| Session limit | 30 minutes |
| Screen-distance threshold | 50 cm |
| Screen-distance alert delay | 10 seconds |

The wearable uses three short buzzer and vibration pulses for posture alerts. A session-limit alert uses two longer pulses.

Local alerts are processed independently of cloud uploads, so a Wi-Fi interruption does not directly prevent local warning signals.

## ☁️ Firebase Database Structure

The system uses Firebase Realtime Database to store live measurements, history, configuration, and alert events.

| Database Path | Purpose |
|---|---|
| `/wearable/latest` | Latest posture and sensor-status readings |
| `/wearable/history` | Periodic posture history and good/bad summaries |
| `/wearable/events` | Posture and session-limit alert events |
| `/settings/wearable` | Remotely configurable wearable settings |
| `/monitor/latest` | Latest screen-distance measurements and status |
| `/monitor/history` | Historical screen-distance readings |
| `/monitor/events` | Screen-distance alert events |

The wearable uploads live readings approximately every 2 seconds and summary records every 60 seconds. The monitor uploads live readings approximately every 3 seconds and history records every 10 seconds.

Records include Firebase server timestamps for historical visualization.

## 📊 Web Dashboard

The PostureSense dashboard is a standalone HTML page that reads data from Firebase and displays the system's current state.

Dashboard features include:

- Wearable and monitor connection status
- Live neck-lean angle and spine-bend percentage
- Screen distance and proximity status
- Session duration and sensor health
- Summary statistics for posture and screen distance
- Historical charts for neck lean, spine bending, and screen distance
- Recent alert-event tables
- Remote wearable configuration controls
- Responsive layout for desktop and mobile screens
- Error messages for database connection and data retrieval issues

The dashboard uses Chart.js to visualize historical measurements.

## 📱 Telegram Notifications

The wearable sends Telegram messages when an alert starts.

Notifications can include:

- The affected posture measurement: neck, spine, or both
- The measured neck-lean angle and spine-bend percentage
- Session-limit alert information

Telegram requests are handled by the wearable's network task, keeping network activity separate from time-sensitive sensing and local alerts.

## 🔌 Hardware Pin Configuration

### Wearable ESP32 Node

| Component | ESP32 Pin | Interface |
|---|---|---|
| MPU6050 SDA | GPIO 21 | I2C |
| MPU6050 SCL | GPIO 22 | I2C |
| Flex sensor output | GPIO 34 | Analog input |
| Buzzer | GPIO 25 | Digital output |
| Vibration motor control | GPIO 26 | Digital output |
| Push button | GPIO 27 | Digital input with internal pull-up |
| On-board LED | GPIO 2 | Digital output |

### Screen Monitor ESP32 Node

| Component | ESP32 Pin | Interface |
|---|---|---|
| HC-SR04 TRIG | GPIO 26 | Digital output |
| HC-SR04 ECHO | GPIO 27 | Digital input |
| Buzzer | GPIO 25 | Digital output |
| LED | GPIO 2 | Digital output |

Check the wiring and voltage levels before powering the circuit. In particular, do not connect a 5 V HC-SR04 echo signal directly to an ESP32 GPIO.

## 🚀 Getting Started

### Prerequisites

Before setting up PostureSense, prepare the following:

- Arduino IDE
- ESP32 board support package
- ArduinoJson library, version 7
- Two ESP32 development boards
- Required sensors and output components
- A 2.4 GHz Wi-Fi network
- A Firebase project with Realtime Database enabled
- A Telegram bot and chat ID if Telegram notifications are required
- A modern web browser

### 1. Clone the Repository

```bash
git clone https://github.com/YOUR_USERNAME/PostureSense.git
cd PostureSense
```

Replace `YOUR_USERNAME` with your GitHub username and `PostureSense` with the actual repository name if different.

### 2. Configure Firebase

1. Create a project in the [Firebase Console](https://console.firebase.google.com/).
2. Enable Realtime Database.
3. Copy the database URL.
4. Configure the database rules for the paths used by the wearable, monitor, and dashboard.
5. Verify that the database accepts the required records.

The project report describes a prototype configuration with open database access. **Do not expose an unrestricted database to the public or use it for real personal data.** Configure authentication and restrictive database rules before deployment.

### 3. Configure the Firmware

Open the firmware files in Arduino IDE:

- `wearable_new.ino`
- `monitor_node_firebase.ino`

Configure the required values in each file, including:

- Wi-Fi SSID
- Wi-Fi password
- Firebase Realtime Database URL
- Telegram bot token and chat ID in the wearable firmware

Use your own credentials. Do not commit passwords, tokens, or private database credentials to GitHub.

### 4. Upload the Firmware

1. Connect the wearable ESP32 to your computer.
2. Select the appropriate ESP32 board and COM port in Arduino IDE.
3. Compile and upload `wearable_new.ino`.
4. Connect the second ESP32.
5. Upload `monitor_node_firebase.ino`.
6. Open the Serial Monitor to check startup messages and sensor status.

### 5. Configure the Dashboard

Open `posturesense_dashboard_separate_neck_spine.html` in a text editor.

Set its Firebase database URL to the same database used by both ESP32 nodes. Save the file and open it in a web browser.

The dashboard should display the latest values after the devices begin uploading data and the database permissions allow the required reads.

### 6. Calibrate and Test

1. Power both ESP32 nodes.
2. Wear the posture-monitoring strap and sit upright.
3. Press the wearable button to begin calibration.
4. Follow the buzzer cues to complete upright capture and guided forward-lean calibration.
5. Open the dashboard and check the connection indicators.
6. Test neck lean and spine bending.
7. Move closer to and farther from the ultrasonic sensor.
8. Verify local alerts, Firebase history, dashboard updates, and Telegram messages.

## 🎛️ Wearable Button Controls

| Button Press Duration | Action |
|---|---|
| Less than 1.5 seconds | Re-zero the posture baseline and start a session |
| 1.5–4 seconds | Stop the session |
| 4 seconds or longer | Perform full calibration |

During full calibration, remain upright during the initial capture and follow the guided forward-lean prompt.

## 🧪 Testing and Validation

The project report defines a testing checklist covering:

- MPU6050 detection and calibration
- Neck-angle measurement
- Flex-sensor response and spine-bend detection
- Motion rejection and sensor-fault recovery
- Posture-alert timing and hysteresis
- Session-limit alerts
- Ultrasonic distance accuracy
- Too-close detection and alert timing
- Firebase data uploads
- Remote wearable settings
- Offline detection and Wi-Fi recovery

Actual accuracy measurements should be recorded using reference instruments and repeated test runs. The design thresholds and timing values listed in this README are firmware defaults, not claims of experimentally measured accuracy.

## ⚠️ Limitations

- The prototype is designed for one user at one desk.
- The ultrasonic sensor can detect other objects in front of it, potentially affecting distance readings.
- The monitor's distance threshold is configured in firmware rather than through the dashboard.
- Sensor accuracy depends on mounting position, calibration, environmental conditions, and hardware quality.
- The prototype requires further work on secure authentication, database permissions, and certificate validation.
- Battery life and wearable portability depend on the final power supply and enclosure.
- The system is not a certified medical device and does not diagnose or treat medical conditions.

## 🔮 Future Improvements

- Add Firebase Authentication and strict, per-device database access rules.
- Enable certificate verification for HTTPS connections.
- Store credentials securely outside publicly shared source code.
- Connect the two ESP32 nodes using ESP-NOW for combined posture and distance decisions.
- Add remote configuration for screen-distance thresholds.
- Improve motion tracking with gyroscope data and sensor fusion.
- Explore machine-learning models for recognizing slouching, leaning, and reclining.
- Develop a rechargeable wearable enclosure with improved power management.
- Add daily and weekly posture reports, adaptive reminders, and multi-user support.
- Implement over-the-air firmware updates.

## 🔐 Privacy and Security

PostureSense does not use a camera or microphone. It records sensor measurements, posture and distance statuses, session information, and timestamps.

However, the prototype's original security configuration has important limitations. The report identifies embedded Wi-Fi and Telegram credentials, disabled HTTPS certificate verification, and open Firebase database rules.

Before sharing or deploying the project:

- Remove all secrets from source code.
- Never publish Wi-Fi passwords or Telegram bot tokens.
- Restrict database reads and writes through authentication and database security rules.
- Enable HTTPS certificate verification.
- Avoid uploading real user monitoring data to a publicly accessible database.

## 📁 Project Structure

The repository contain the following files:

```text
PostureSense/
├── wearable_new.ino
├── monitor_node_firebase.ino
├── posturesense_dashboard_separate_neck_spine.html
├── firebase_rules.json
├── README.md
└── screenshots/
    ├── architecture.png
    ├── wearable_circuit.png
    ├── monitor_circuit.png
    └── screenshots/
        ├── dashboard.png
        └── alert_history.png
```

This is a suggested repository structure. Add the `docs/` folder and image files only if you include those assets in your repository.

## 🎓 Academic Context

**Module:** CCS 4062 – Embedded Systems and Internet of Things  
**Institution:** University of Sri Jayewardenepura  
**Project:** PostureSense – Smart Posture and Screen Distance Monitoring System  
**Project Type:** Embedded Systems and IoT Prototype  
**Date:** October 2026

## 📚 References

- Stincel, O.-R. et al. (2023). *Assessment of Forward Head Posture and Ergonomics in Young IT Professionals – Reasons to Worry?* La Medicina del Lavoro, 114(1), e2023006. https://doi.org/10.23749/mdl.v114i1.13600
- Rosenfield, M. (2011). *Computer vision syndrome: a review of ocular causes and potential treatments.* Ophthalmic and Physiological Optics, 31(5), 502–515.
- InvenSense. (2013). *MPU-6000 and MPU-6050 Product Specification, Revision 3.4.*
- Google. [Firebase Realtime Database REST API Documentation](https://firebase.google.com/docs/database/rest/start)
- Telegram. [Telegram Bot API](https://core.telegram.org/bots/api)
- Chart.js. [Chart.js Documentation](https://www.chartjs.org/)

---

**PostureSense — Making healthier desk habits easier through embedded sensing and IoT.**
