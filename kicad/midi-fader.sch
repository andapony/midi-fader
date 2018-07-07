EESchema Schematic File Version 2
LIBS:power
LIBS:device
LIBS:switches
LIBS:relays
LIBS:motors
LIBS:transistors
LIBS:conn
LIBS:linear
LIBS:regul
LIBS:74xx
LIBS:cmos4000
LIBS:adc-dac
LIBS:memory
LIBS:xilinx
LIBS:microcontrollers
LIBS:dsp
LIBS:microchip
LIBS:analog_switches
LIBS:motorola
LIBS:texas
LIBS:intel
LIBS:audio
LIBS:interface
LIBS:digital-audio
LIBS:philips
LIBS:display
LIBS:cypress
LIBS:siliconi
LIBS:opto
LIBS:atmel
LIBS:contrib
LIBS:valves
LIBS:midi-fader
LIBS:midi-fader-cache
EELAYER 25 0
EELAYER END
$Descr A4 11693 8268
encoding utf-8
Sheet 1 4
Title ""
Date ""
Rev ""
Comp ""
Comment1 ""
Comment2 ""
Comment3 ""
Comment4 ""
$EndDescr
$Sheet
S 1200 1350 4800 3650
U 5B3C3C50
F0 "Digital Control" 60
F1 "digital-control.sch" 60
F2 "IN[0..5]" I R 6000 2000 60 
F3 "FADER_A" I R 6000 1550 60 
F4 "FADER_B" I R 6000 1700 60 
$EndSheet
$Sheet
S 1200 5400 4800 1300
U 5B3C3C5C
F0 "Power Supplies" 60
F1 "power-supplies.sch" 60
$EndSheet
$Sheet
S 6400 1300 2300 3700
U 5B4048F0
F0 "Input" 60
F1 "input.sch" 60
F2 "IN[0..5]" I L 6400 2000 60 
F3 "FADER_A" I L 6400 1550 60 
F4 "FADER_B" I L 6400 1700 60 
$EndSheet
Wire Wire Line
	6000 1550 6400 1550
Wire Wire Line
	6400 1700 6000 1700
Wire Bus Line
	6000 2000 6400 2000
$EndSCHEMATC
