
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

PORT = 7990
IFACES = []
DEBUG = True
MAX_CONNECTIONS = 10

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

"""
try:
  HOST = netifaces.ifaddresses('eth0')[netifaces.AF_INET][0]['addr']
  try: #disable stdout i/o buffering
    sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', 0)
  except: pass
except:
  HOST = "127.0.0.1"
  for i in netifaces.interfaces():
    try:
      ip = netifaces.ifaddresses(i)[netifaces.AF_INET][0]['addr']
      if IP_PREFIX in ip:
        HOST = ip;
        break
    except: pass
"""

sp = None
serial_types = ["COM", "/dev/ttyUSB", "/dev/ttyACM"]
serial_type = 1
serial_port = 0

def serveClient(client, command_buff, sp):
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
        if DEBUG:
          print ("Writing to Arduino: " + command),
        sp.write(command.encode('UTF-8'))
    return command_buff
  except Exception as e:
    print ("Failed to read from client, error: " + e.msg)
    return None

while True:
  try:
    sp = serial.Serial()
    sp.baudrate = 9600
    sp.port = serial_types[serial_type] + str(serial_port)
    sp.open()
    print ('Connected to Arduino on serial')
    time.sleep(1)
  except Exception as e:
    print ('Failed to initiate serial communication: ' + str(e))
    time.sleep(0.1)
    serial_port += 1
    if serial_port == 8:
      serial_port = 0
      serial_type = (serial_type + 1) % len(serial_types)
      if serial_type == 0 and serial_port < 2:
        serial_port = 2
    continue

  s = None
  try:
    connections = []
    sp.flushInput()
    sp_buffer = bytearray([])
    server_socks = []

    find_interfaces()
    for iface in IFACES:
      try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
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
          buffers[str(conn)] = ""
        else:
          key = str(sock)
          try:
            newbuf = serveClient(sock, buffers[key], sp)
          except:
            newbuf = None
          if newbuf is not None:
            buffers[key] = newbuf
          else:
            print ('closing connection')
            del buffers[key]
            sock.close()
            connections.remove(sock)

      try:
        while sp.inWaiting():
          b = sp.read()
          sp_buffer += b
          if b == chr(255):
            break
        if len(sp_buffer) > 0:
          try:
            if DEBUG:
              arr = []
              for i in range (0, len(sp_buffer)-1):
                arr += [sp_buffer[i]]
              #print ("Values read from Arduino: " + str(arr))
            # broadcast to all connections
            for sock in connections:
              if sock not in server_socks:
                sock.send(sp_buffer)
          except socket_error as serr:
            print (serr)
            if serr.errno != errno.EAGAIN:
              raise
          sp_buffer = bytearray([])
      except:
        raise

      num_interfaces = len(IFACES)
      find_interfaces()
      if len(IFACES) != num_interfaces:
        # we have new interfaces up!
        print ("Found new interfaces, will restart by crashing...")
        raise
  except: print ('Crashed, restarting...')
  finally:
    print ('Killing all sockets')
    for sock in connections:
      try:
        sock.close()
      except: pass
    connections = []
  time.sleep(2)
