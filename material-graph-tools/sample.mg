material_graph v1
# nodes
node 1 ConstantFloat 0 0
node 2 ConstantFloat 0 120
node 3 Multiply 200 60
node 4 ConstantFloat 0 240
node 5 Add 200 150
node 6 ConstantFloat 0 360
node 7 OneMinus 200 300
node 8 Lerp 400 120
node 9 Output 600 60
# pins
pin 1 1 out float 1
pin 2 2 out float 1
pin 3 3 a float 0
pin 3 4 b float 0
pin 3 5 out float 1
pin 4 6 out float 1
pin 5 7 a float 0
pin 5 8 b float 0
pin 5 9 out float 1
pin 6 10 out float 1
pin 7 11 x float 0
pin 7 12 out float 1
pin 8 13 a float 0
pin 8 14 b float 0
pin 8 15 t float 0
pin 8 16 out float 1
pin 9 17 baseColor float 0
# props
prop 1 value 0.8
prop 2 value 0.5
prop 4 value 0.2
prop 6 uname uTint
prop 6 uniform 1
prop 6 value 0.25
# edges
edge 1 1 1 3 3
edge 2 2 2 3 4
edge 3 4 6 5 7
edge 4 3 5 5 8
edge 5 4 6 7 11
edge 6 5 9 8 13
edge 7 7 12 8 14
edge 8 6 10 8 15
edge 9 8 16 9 17
