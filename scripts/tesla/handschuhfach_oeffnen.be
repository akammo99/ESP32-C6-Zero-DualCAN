# @name Tesla glovebox
# @description Opens the Tesla glovebox using the vehicle CAN bus.
# @bus 0
#
# Source: DashKit vehicle_control.c + bus_1_tesla_vehicle.dbc
# TX: UI_vehicleControl2, ID 0x3B3
#   UI_gloveboxRequest: 0=IDLE, 1=OPEN (latch release)
#
# DashKit behavior: assert for ~400 ms in 20 ms intervals,
# then send five release frames at 20 ms intervals.

def send_glovebox(value)
  var msg = can_msg_get(0, "UI_vehicleControl2")
  if msg == nil
    msg = can_msg_new("UI_vehicleControl2")
  end
  msg_sig_set(msg, "UI_gloveboxRequest", value)
  can_msg_send(0, msg)
end

def glovebox_release(remaining)
  if remaining <= 0
    print("Glovebox pulse complete")
    led_off()
    return
  end
  send_glovebox(0)
  timer_after(20, /-> glovebox_release(remaining - 1))
end

def glovebox_hold(remaining)
  if remaining <= 0
    glovebox_release(5)
    return
  end
  send_glovebox(1)
  timer_after(20, /-> glovebox_hold(remaining - 1))
end

def open_glovebox()
  print("Tesla glovebox: open request")
  led_set(0, 0, 40)
  glovebox_hold(20)
end

def setup()
  action_register("open_glovebox", /-> open_glovebox())
  print("Tesla glovebox tile loaded")
end
