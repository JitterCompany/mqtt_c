# mqtt_c: A non-blocking state-machine based MQTT client

This package is an abstraction over the paho MQTTPacket API: See https://github.com/eclipse/paho.mqtt.embedded-c/blob/master/MQTTPacket

The goal is to provide a higher abstraction layer than MQTTPacket,
but nonblocking and very flexible plain-c (targeted at embedded systems).
