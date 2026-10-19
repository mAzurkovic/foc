# Controller for Hobby PMSMs

This document acts as a "product requirements document" (PRD) for a generic PMSM motor controller.

## Requirements

### Functional

* The controller must be able to control position, speed, and torque of the motor.

* The controller must support any generic hobby PMSM (as long as it is within the controllers rated voltage and current limits).

* The controller must support voltages from 12-48V.

* The controller must output currents up to X amps.

* The controller must be daisy-chainable with other controllers (power and communication). For example, in a robotic arm use-case, each motor does not need a wire harness back to main control board.

* Users must supply motors poles, kV, voltage max, current rating, interia, and reducer ratio (if reducer present).

* The controller must support "Configuration mode", a set of commands which.
   - Set up: a command that determines the motor's winding resistance and inductance (in order to determine P and I gains).
   - Manual tuning: a command that lets users sets speed and position loop gains.

* The controller must support "Operation mode": 
    - Position mode: the controller drives motor to given position setpoint. 
    - Speed mode: the controller drives motor to given speed setpoint. 
    - Torque mode: the controller drives motor to given torque setpoint. 

* The controller must support a USB-C connection for "Configuration mode" and "Operation mode" as a single motor (no daisy-chain support via USB-C).