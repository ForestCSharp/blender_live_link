# Blender Live Link #

An experiment in using Blender as the primary tooling for a 3D game, inspired by Santa Monica Studio's 2024 REAC Presentation [Maya as Editor: The game development approach of Santa Monica Studio](https://www.youtube.com/watch?v=ZwPogOhbNWw)

This repository contains two key components.
1. A blender extension written in Python that sends relevant state across the net (see: \__init__.py)
2. A game written in C++ that listens for state updates from blender.

## Building ##
Blender Live Link has been tested on Mac and Windows. Building is done by calling build.sh in the root directory, so you'll need a way of running shell scripts on Windows (I use git bash).
My primary development machine is currently a Mac, but I try to test on Windows occasionally to ensure feature-parity.

