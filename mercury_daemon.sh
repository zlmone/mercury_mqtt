#!/bin/sh
killall mercury236mqtt
nohup ./mercury236mqtt /dev/ttyUSB0 &
