# Smart Water Bottle — IoT Mini Project

[![Board](https://img.shields.io/badge/Board-ESP8266%20NodeMCU-blue)](https://www.espressif.com/)
[![Sensor](https://img.shields.io/badge/Sensor-HX711%20%2B%20Load%20Cell-green)](https://www.hx711.com/)
[![Protocol](https://img.shields.io/badge/Protocol-MQTT-orange)](https://mqtt.org/)

โครงการนี้เป็นส่วนหนึ่งของ **Mini Project รายวิชา IoT (Internet of Things)** สำหรับนักศึกษาภาควิชาวิศวกรรมคอมพิวเตอร์ (Computer Engineering - CPE) 

ระบบ **Smart Water Bottle** ถูกออกแบบมาเพื่อตรวจจับ ติดตาม และบันทึกปริมาณการดื่มน้ำในชีวิตประจำวันแบบ Real-time โดยใช้อุปกรณ์ฮาร์ดแวร์วัดน้ำหนัก ประมวลผลด้วย State Machine เพื่อกรอง สัญญาณรบกวน (Noise Filter) และส่งข้อมูลผ่านโปรโตคอล MQTT เข้าสู่ Web Dashboard และแจ้งเตือนผ่าน LINE Messaging API

---

## ฟีเจอร์หลัก (Key Features)

- **Pure State Machine Architecture:** จัดการสถานะการทำงานด้วย Finite State Machine (`NO_BOTTLE`, `SETTLING`, `IDLE`) ช่วยแยกแยะสถานะการวางขวด ยกขวด และรอน้ำหนักนิ่งได้อย่างแม่นยำ
- **Noise Filtering & Event Detection:** มีอัลกอริทึมกรอง สัญญาณขยะจาก Load Cell และจำแนกพฤติกรรมผู้ใช้ระหว่าง **"การจิบน้ำ" (Drink)** กับ **"การเติมน้ำ" (Refill)** อัตโนมัติ
- **Wi-Fi Provisioning:** เชื่อมต่อ Wi-Fi ผ่าน **WiFiManager** (Captive Portal) โดยไม่ต้อง Hardcode ชื่อรหัสผ่านลงในตัวโปรแกรม
- **Real-time Web Dashboard:** หน้าเว็บ Dashboard (HTML/JS/Chart.js) แสดงผลกราฟเปอร์เซ็นต์ ระดับน้ำในขวด กราฟสรุปประวัติย้อนหลัง และ Log เหตุการณ์แบบ Real-time
- **Automation & Push Notification:** รองรับการเชื่อมต่อกับ **Node-RED** และ **LINE Messaging API** เพื่อส่งข้อความแจ้งเตือนเข้าโทรศัพท์มือถือเมื่อไม่ได้ดื่มน้ำตามระยะเวลาที่กำหนด

---

## อุปกรณ์และเทคโนโลยีที่ใช้ (Hardware & Software)

### Hardware
- **Microcontroller:** ESP8266 (NodeMCU v2/v3)
- **Weight Sensor:** Load Cell (รองรับพิกัดน้ำหนักขวดน้ำ) + HX711 Module
- **Power Supply:** USB 5V

### Software & Protocols
- **Development Environment:** VS Code + PlatformIO (C/C++)
- **Communication Protocol:** MQTT (`broker.emqx.io`)
- **Libraries Used:** `PubSubClient`, `HX711`, `WiFiManager`, `EEPROM`
- **Frontend Dashboard:** HTML5, CSS3, JavaScript (ES6), Chart.js, MQTT.js

---

## สถาปัตยกรรมระบบ (System Architecture)

```text
[ Load Cell + HX711 ]
          │
          ▼ (Analog Signals)
   [ ESP8266 NodeMCU ] ──(FSM & Processing)
          │
          ▼ (MQTT Protocol via WebSocket/TCP)
   [ MQTT Broker: broker.emqx.io ]
          ├──► [ Web Dashboard (index.html) ]
          └──► [ Node-RED ] 
