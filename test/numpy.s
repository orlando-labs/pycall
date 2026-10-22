package pycall.numpy_test
import pycall.numpy as np
export main

def main():
  array = np.array([1, 2, 3], dtype: "int64")
  raise ValueError("NumPy conversion") unless array.tolist().to_a == [1, 2, 3]
  raise ValueError("NumPy reduction") unless array.sum().invoke("__int__") == 6
  matrix = np.array([[1, 2], [3, 4]])
  raise ValueError("NumPy shape") unless matrix.shape.to_a == [2, 2]
  print("PASS pycall NumPy")
  0
