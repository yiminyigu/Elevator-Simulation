# Elevator-Simulation

This is a elevator-simulation project that focus on realness in terms of the most common elevators we see in our daily life. 

## How to run
----Compile the source file on Windows (with window.h library )and run----

For rendering, the project uses window.h console facilities. The elevators are drawn horizontally because we can have more spaces for each floor and thus to be more expressive. For each floor, ‘DN’ and ‘UP’ indicate the request buttons and the color indicates whether it is pressed or not. The number below indicates the number of people in line for that direction.

<img src="/Elevator-Simulation/"

## Whole View
The project includes three parts: 
People-feed: respond to generate wait people for each floor at certain time.
People-In-Line: floor manager thread at each floor get wait people vector, put them in line and generate up/down request.
People-consume: each cart thread goes to certain floor based on the current passengers’ target floor and floor request and load/unload people if applicable.
----people feed data sequence can be tracked in datafeed.txt, if you want to repeat the process with same people feed data, comment the first feedData() in main function. Or if you change some relevant parameters below, uncomment that line to generate new people feed data.----
----Final statistical data is recorded in err.txt, the three column represents: Total people picked, total wait time and total running time(both in second).
[This is a sample statistics result based on the setting below.]


 
