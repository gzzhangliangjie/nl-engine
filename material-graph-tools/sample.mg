material_graph v1
# nodes
node 1 ConstantFloat 0 0
node 2 ConstantFloat 0 120
node 3 Multiply 200 60
node 4 Output 400 60
# pins
pin 1 1 out float 1
pin 2 2 out float 1
pin 3 3 a float 0
pin 3 4 b float 0
pin 3 5 out float 1
pin 4 6 baseColor float 0
# props
prop 1 value 0.8
prop 2 value 0.5
# edges
edge 1 1 1 3 3
edge 2 2 2 3 4
edge 3 3 5 4 6
