import socket
print('DNS:')
print(socket.gethostbyname('mqtt.xiaozhi.me'))
s=socket.socket()
s.settimeout(5)
try:
    s.connect(('mqtt.xiaozhi.me', 8883))
    print('Connected: True')
except Exception as e:
    print('Error:', e)
finally:
    s.close()
