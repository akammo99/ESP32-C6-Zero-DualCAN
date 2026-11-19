# @name Spiegel einklappen / ausklappen
# @description Schaltet anhand des tatsächlichen Spiegelstatus
# @bus 0

var send_count = 0
var tx_msg = nil

def send_frame()
  if tx_msg == nil
    return
  end

  can_msg_send(0, tx_msg)
  send_count += 1

  if send_count < 10
    timer_after(20, send_frame)
  end
end

def setup()
  var status = can_msg_get(0, "VCLEFT_doorStatus")
  if status == nil
    print("Spiegelstatus nicht verfügbar")
    return
  end

  var state = msg_sig_get(status, "VCLEFT_mirrorFoldState")
  if state == nil
    print("Mirror state nicht verfügbar")
    return
  end

  var target = -1

  if state == 1
    target = 2
  elif state == 2
    target = 1
  else
    print("Spiegel bewegt sich oder Status unbekannt")
    return
  end

  var msg = can_msg_get(0, "UI_vehicleControl")
  if msg == nil
    print("VehicleControl nicht verfügbar")
    return
  end

  msg_sig_set(msg, "UI_mirrorFoldRequest", target)
  tx_msg = msg
  send_count = 0
  send_frame()

  print("Spiegel: " + ("AUSKLAPPEN" if target == 2 else "EINKLAPPEN"))
end
