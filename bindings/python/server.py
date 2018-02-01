#!/opt/local/bin/python

from pmix import *
import signal, time

global killer

class GracefulKiller:
  kill_now = False
  def __init__(self):
    signal.signal(signal.SIGINT, self.exit_gracefully)
    signal.signal(signal.SIGTERM, self.exit_gracefully)

  def exit_gracefully(self,signum, frame):
    self.kill_now = True

def clientconnected(args:dict is not None):
    print("CLIENT CONNECTED", args)
    return

def main():
    try:
        foo = PMIxServer()
    except:
        print("FAILED TO CREATE SERVER")
        exit(1)
    print("Testing server version ", foo.get_version())
    args = {'FOOBAR': 'VAR', 'BLAST': 7}
    map = {'clientconnected': clientconnected}
    my_result = foo.init(args, map)
    while True:
        time.sleep(1)
        if killer.kill_now:
            break
    print("FINALIZING")
    foo.finalize()

if __name__ == '__main__':
    global killer
    killer = GracefulKiller()
    main()
