
'''
  Simple socket server using threads
'''

import socket
import sys, os
import time
import serial
import errno
from socket import error as socket_error
import netifaces
import select

cur_order_id = 1
class Order:
  def __init__(self, s, o, c, i, r):
    global cur_order_id
    self.session = s
    self.id = cur_order_id
    cur_order_id += 1
    self.order = o
    self.room = r
    self.count = c
    self.index = i

ORDERS = []
MENU = []
ACCEPTANCE_HISTORY = []
MAX_HISTORY_SIZE = 500
unique_order_id = 0

PORT = 7992
IFACES = []
DEBUG = True
MAX_CONNECTIONS = 10

try:
  with open("kitchen_menu", "r") as f:
    MENU = f.readlines()
    f.close()
except: pass
MENU = map(lambda f: f.replace('\n', ''), MENU)
for f in MENU:
  if len(f) == 0:
    MENU.remove(f)

def find_interfaces():
  global IFACES
  for i in netifaces.interfaces():
    try:
      ip = netifaces.ifaddresses(i)[netifaces.AF_INET][0]['addr']
      sip = ip.split('.')
      if (int(sip[0]) > 160 and int(sip[0]) < 250 and int(sip[1]) != 254) or (int(sip[0]) == 10 and int(sip[1]) == 10):
        if i not in IFACES: # best be a new interface
          IFACES += [i]
          print (('Address: %s:%d') % (ip, PORT))
    except: pass

def sendMenu(client):
  global MENU
  buffer = bytearray([255, 255, 255, len(MENU)])
  for i in range(0, len(MENU)):
    b = bytearray(map(lambda x: MENU[i][x] if x < len(MENU[i]) else '\0', range(0, 128)))
    buffer += b
  client.send(buffer)

def serveClient(client, command_buff, connections):
  global ORDERS, MENU, ACCEPTANCE_HISTORY, MAX_HISTORY_SIZE, unique_order_id
  try:
    tmp = client.recv(1024)
    if not tmp:
      return None
    command_buff += tmp.decode('UTF-8')
    while '\n' in command_buff:
      pos = command_buff.find('\n')
      command = command_buff[0:pos+1]
      command_buff = command_buff[pos+1:]
      if command != "S\n":
        command = command.replace('\r', '')
        command = command.replace('\n', '')
        if DEBUG:
          print(str(client) + ": " + command)
        if "getorders" in command:
          # format: "getorders"
          s = ""
          for O in ORDERS:
            s += "orders:" + str(O[0]) + ":" + str(O[1])
            for o in O[2:]:
              s += ";"+ str(o.session) + ":" + str(o.id) + ":" + str(o.order) + ":" + str(o.count)
            s += "\n"
          if (s == ""):
            s = "\n"
          try:
            client.send(s.encode('UTF-8'))
          except: pass
        elif "setorder:" in command:
          # format: "setorder:<session>:<id>:<0 if rejected, 1 if accepted>"
          order = command.split(':')
          if len(order) == 4:
            for room_order in ORDERS:
              for o in room_order[2:]:
                if o.session == int(order[1]) and o.id == int(order[2]):
                  for conn in connections:
                    if conn != client: # sender of "setorder" (client) is kitchen
                      try:
                        print ("sending to client..")
                        conn.send(bytearray([o.session % 256, o.index, int(order[3])]))
                        ACCEPTANCE_HISTORY += [[order[0], order[1], str(o.index), order[3]]]
                        if len(ACCEPTANCE_HISTORY) > MAX_HISTORY_SIZE:
                          ACCEPTANCE_HISTORY = ACCEPTANCE_HISTORY[1:]
                      except: pass
                  room_order.remove(o)
                  if len(room_order) == 2: # no more orders
                    ORDERS.remove(room_order)
        elif "whatabout:" in command:
          # format: "whatabout:<session>"
          session = int(command.split(':')[1])
          for c in ACCEPTANCE_HISTORY:
            if int(c[1]) == session:
              try:
                client.send(bytearray([session % 256, int(c[2]), int(c[3])]))
              except: pass
        elif "order:" in command: # someone is ordering something
          # format: "order:<name>:<session>:<num_orders>:<num_items>x<item_id>:..."
          orders = command.split(':')
          if len(orders) > 3:
            sender_name = orders[1]
            session = int(orders[2])
            orders = orders[4:]
            i = 0
            room_order = [unique_order_id, sender_name]
            unique_order_id += 1
            for o in orders:
              order = o.split('x')
              room_order += [Order(session, MENU[int(order[1])], int(order[0]), i, sender_name)]
              i += 1
            ORDERS += [room_order]
    return command_buff
  except Exception as e:
    print ("Failed to read from client, error: " + str(e))
    return None

while True:
  try:
    connections = []
    server_socks = []

    find_interfaces()
    for iface in IFACES:
      try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        print ('Socket created')

        #Bind socket to local host and port
        try:
          ip = netifaces.ifaddresses(iface)[netifaces.AF_INET][0]['addr']
          s.bind((ip, PORT))
        except socket_error as msg:
          print ('Bind failed. Error Code : ' + str(msg))
          #if msg.errno == errno.EADDRINUSE: # already in use...
          raise

        print ('Socket bind complete')

        #Start listening on socket
        s.listen(MAX_CONNECTIONS)
        print ('Socket now listening')
        server_socks += [s]
      except:
        try: s.close()
        except: pass
        pass
    if server_socks == []:
      raise

    connections += server_socks
    buffers = {}

    #now keep talking with the client
    while 1:
      ready_sockets,outputready,exceptready = select.select(connections,[],[], 0.5)

      for sock in ready_sockets:
        if sock in server_socks:
          conn, addr = sock.accept()
          print ('Connected with ' + addr[0] + ':' + str(addr[1]))
          connections.append(conn)
          if not "10.10." in str(addr):
            sendMenu(conn) # if not the kitchen, send the menu
          buffers[str(conn)] = ""
        else:
          key = str(sock)
          try:
            newbuf = serveClient(sock, buffers[key], [item for item in connections if item not in server_socks])
          except Exception as e:
            print (str(e))
            newbuf = None
          if newbuf is not None:
            buffers[key] = newbuf
          else:
            print ('closing connection')
            del buffers[key]
            sock.close()
            connections.remove(sock)

      num_interfaces = len(IFACES)
      find_interfaces()
      if len(IFACES) != num_interfaces:
        # we have new interfaces up!
        print ("Found new interfaces, will restart by crashing...")
        raise
  except Exception as e: print ('Crashed, restarting... (' + str(e) + ')')
  finally:
    print ('Killing all sockets')
    for sock in connections:
      sock.close()
    connections = []
  time.sleep(2)
