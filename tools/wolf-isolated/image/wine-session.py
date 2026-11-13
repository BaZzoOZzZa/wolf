#!/usr/bin/python3 -I
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, '/opt/wolf')
import guest
try:
    sys.exit(guest.wine_session(dict(__import__('os').environ), sys.argv[1:]))
except (guest.Refused, OSError):
    sys.exit('MWOW-9 Wine session refused')
