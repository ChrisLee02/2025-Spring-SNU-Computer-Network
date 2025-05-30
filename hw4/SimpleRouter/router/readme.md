# README: Project 4 – Simple Router

**Student ID:** 2021-18641  
**Name:** Hadong Lee  

---

| Test ID | Command | Description |
|---------|---------|-------------|
| **T1** | `client1 ping 192.168.2.2` | Tests basic ARP broadcast/reply exchange and ICMP Echo Reply generation. Confirms that the router responds to echo requests directed to its interface and properly resets TTL. |
| **T2** | `client1 traceroute 192.168.2.2` | Verifies TTL decrement at each hop and correct generation of ICMP Time Exceeded messages from the router during intermediate forwarding. |
| **T3** | `client1 ping 8.8.8.8` | Tests behavior when destination is unreachable due to no matching route in the routing table. Router must respond with ICMP Network Unreachable. |
| **T4** | `client1 ping 192.168.2.99` | Ensures ARP retry logic is triggered and fails after 5 attempts, followed by correct generation of ICMP Host Unreachable. |
| **T5** | `client1 wget 10.0.1.1` | Sends UDP to a closed port on the router to test generation of ICMP Port Unreachable when transport protocol reaches an unsupported endpoint. |
| **T6** | `client2 ping 10.0.1.100` | Tests enforcement of IP blacklist policy for source IPs within 10.0.2.0/24. Packet should be dropped silently with router's logs. |
| **T7** | `client1 ping 10.0.2.100` | Tests enforcement of IP blacklist policy for destination IPs within 10.0.2.0/24. Packet should be dropped silently with router's logs. |

## Test 1 result
![alt text](image.png)
![alt text](image-1.png)
![alt text](image-2.png)

## Test 2 result
![alt text](image-3.png)
![alt text](image-4.png)
![alt text](image-5.png)

## Test 3 result
![alt text](image-6.png)
![alt text](image-7.png)

## Test 4 result
![alt text](image-8.png)
![alt text](image-9.png)

## Test 5 result
![alt text](image-10.png)
![alt text](image-11.png)

## Test 6 result
![alt text](image-12.png)
![alt text](image-13.png)
![alt text](image-14.png)

## Test 7 result
![alt text](image-15.png)
![alt text](image-16.png)

## Inital TTL
![alt text](image-17.png)
