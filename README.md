Multiple-MOC-Power-Module-control-by-BLE
you can controll maximum 4 BLE power module from ESP32 through MQTT
you need to config in arduino code
  - WIFI connection
  - MQTT Broker
  - Power Module Name (you can use BT SCANNER APP on mobile for scan device name)

for Debug, you can sent command on serial that connect to esp32 by this
<device>,<AB mode>,<AB speed>,<CD mode>,<CD speed>
example: 1,1,100,0,0 
that mean control device no.1, port A/B forward, speed = 100, port C/D stop, speed = 0

for MQTT subscrib payload
you need to sent in json format
example: {"device":1,"AB_mode":1,"AB_speed":10,"CD_mode":0,"CD_speed":0}
that mean control device no.1, port A/B forward, speed = 10, port C/D stop, speed = 0

and you can control all device in the same time by use device no. 255 (broadcast)
