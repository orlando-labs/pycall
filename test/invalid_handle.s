package pycall.invalid_handle
import pycall
export main

def main():
  disposed = pycall.wrap([1])
  disposed.destroy!()
  pycall.callback(|x|: disposed)(5)
