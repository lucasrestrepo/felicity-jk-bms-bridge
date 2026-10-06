# GitHub setup (delete this file after publishing)

Repository name: `felicity-jk-bms-bridge`

Description (About box):
> ESP32 bridge that lets Felicity Solar inverters talk to JK BMS over RS485 (Pylontech protocol address translation) with a live web dashboard and OTA.

Topics (paste into the About → Topics box):
```
esp32 esp32-s3 arduino rs485 max485 pylontech pylontech-protocol felicity felicity-solar
jk-bms jkbms bms battery-management-system lifepo4 solar off-grid inverter
solar-inverter energy-monitoring ota home-automation esphome
```

Publish with GitHub CLI:
```bash
gh auth login
gh repo create lucasrestrepo/felicity-jk-bms-bridge --public --source=. --remote=origin \
  --description "ESP32 bridge that lets Felicity Solar inverters talk to JK BMS over RS485 (Pylontech protocol address translation) with a live web dashboard and OTA." \
  --push
gh repo edit lucasrestrepo/felicity-jk-bms-bridge --add-topic esp32,esp32-s3,arduino,rs485,max485,pylontech,pylontech-protocol,felicity,felicity-solar,jk-bms,jkbms,bms,battery-management-system,lifepo4,solar,off-grid,inverter,solar-inverter,energy-monitoring,ota,home-automation,esphome
```
