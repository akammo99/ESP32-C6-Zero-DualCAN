# @name Tesla trunk and frunk
# @description Opens the rear trunk or front trunk (frunk).
# @bus 0
#
# Source: DashKit vehicle_control.c / vehicle_control.h
# DBC: bus_1_tesla_vehicle.dbc
# CAN0 = Vehicle
# TX: UI_vehicleControl, CAN-ID 0x273 (627)
#   UI_remoteClosureRequest: 1=REAR_TRUNK, 2=FRONT_TRUNK
#
# DashKit sends a burst of 10 frames with 20 ms spacing.
# This script reproduces that burst. It does not implement door unlatching.

def send_closure(value, remaining)
  var msg = can_msg_get(0, "UI_vehicleControl")
  if msg == nil
    msg = can_msg_new("UI_vehicleControl")
  end
  msg_sig_set(msg, "UI_remoteClosureRequest", value)
  can_msg_send(0, msg)

  if remaining > 1
    timer_after(20, /-> send_closure(value, remaining - 1))
  else
    print("Tesla closure request sent: " .. str(value))
    led_off()
  end
end

def open_rear_trunk()
  print("Tesla rear trunk: OPEN request")
  led_set(0, 0, 40)
  send_closure(1, 10)
end

def open_frunk()
  print("Tesla frunk: OPEN request")
  led_set(0, 0, 40)
  send_closure(2, 10)
end

def setup()
  action_register("open_rear_trunk", /-> open_rear_trunk())
  action_register("open_frunk", /-> open_frunk())
  print("Tesla trunk/frunk tiles loaded")
end
