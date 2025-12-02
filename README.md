This is a room thermostat from at least 2 esp32 pcb.Big advantage of this system is that it use wifi local connection and AP to setup it using WEB UI.
It have many options to calibrate temperature and humidity, set up four schedule program and a graph history.
Another advantage of it is that it can support maximum 8 sensor from local Wifi.In Web UI you'll find that option to add/remove a new sensor and the check box that can make which sensor can be the main sensor who will be in control of the relay.
Esp32+relay is the brain of this configuration,it listening on port 80.you cand open a port from your rooter and acces if from outside of your house.
The big thing: NO need screw, or iron soldering, you will need only a few drops of superglue.It is not necessary to unsolder the pins from PCBs.You will can use them for the future projects.
Poll interval sensor is 60 seconds, it is permanent parameter.
Debounce is 60 seconds by default, and it can be modify.
You can install a button to factory reset using Pin 5, or you can find Factory reset button on web UI.
Another thing is about you can set offset to make correction directly on esp32+aht1 sensor UI or on relay board UI via wifi.

1st: esp32+relay which command your heater
  Pin connection from relay to esp board :
 IN -> pin 25
 GND -> GND
 VCC -> VN +5V

2nr esp32 + aht10 temp/hum sensor
  Pin connection from aht10 to esp board :
VIN ->3V3 +3V
GND -> GND
SCL -> G22
SDA -> G21

Easy method to flash esp32(Flash download Tool):
load xxxx.bootloader.bin Address: 0x1000
load xxxx.partition.bin  Address: 0x8000
load xxxx.ino.bin        Address: 0x10000

After flashing need to reset the board!!! after that AP will appear and you can change connection to your local wifi network!

Or you can use other IDE (Arduino IDE, etc..) and use the code to upload it.
It is nice because all code is formed in one file, and it is easy for everyone to flash it.

You cand find in this place the *.3mf project for the case to print it for both: pcb+relay & pcb+aht10 sensor.

Material:Pla
Can be print with infill 20%
any printer any speed
recomanded bottom part to be inclined at 60degrees using manual supports and activate autobrim!

TO DO:
manual increase/decrease sensor polling time
OTA firmare update

Goodluck
