# Modern Monopoly

A smart Monopoly system that automatically tracks board state 
and enforces rules, so players can focus on strategy and fun.

## Project Status
🚧 Work in progress — Milestone 1

## System Overview
- Board Sensing: RFID / pressure sensor / hall effect sensor
- Vision Detection: Camera + Grove Vision AI V2
- Display: Central bank screen + player devices

## Repository Structure
- docs/ — milestone reports
- sensing/ — sensor prototyping code
- vision/ — model training and deployment
- display/ — screen UI code
- game-logic/ — game state and rules engine

## Team
- Sienna — board sensing, vision detection
- Polly — player device, game logic

## Hardware
- XIAO ESP32-S3
- Grove Vision AI V2
- RC-522 RFID readers
- FSR pressure sensors
- Linear hall effect sensors
- ESP32-S3 LCD 4.3"
- OLED SSD1351