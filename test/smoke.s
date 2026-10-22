package pycall.smoke
import pycall.math as math
from pycall.json import dumps
import pycall
import pycall.builtins as builtins
import task
export main

def check(value):
  raise ValueError("pycall assertion failed") unless value

def main():
  check(math.sqrt(81) == 9.0)
  check(dumps([1, 2, 3]) == "[1, 2, 3]")
  values = pycall.wrap([1, 2, 3])
  check(values[1] == 2)
  values[1] = 9
  check(values.to_a == [1, 9, 3])
  check(math.pi > 3.14)
  check(pycall.wrap({name: "Ada"})["name"] == "Ada")
  captured = 7
  fn = pycall.callback(|x|: x + captured)
  check(fn(5) == 12)
  check(builtins.sorted([3, 1, 2], key: pycall.callback(|x|: 0 - x)).to_a == [3, 2, 1])
  math_alias = pycall.import_module("math")
  check(builtins.id(math_alias) == builtins.id(math))
  caught = false
  try:
    math.sqrt(-1)
  rescue pycall.Error |e|:
    caught = true
  check(caught)
  child = task.spawn:
    local = task.local()
    local.with(77):
      body = |x|:
        task.sleep(0.001)
        x + local.get()
      worker_callback = pycall.callback(body)
      worker_callback(5)
  check(child.wait() == 82)
  print("PASS pycall smoke")
  0
