
/* static void
recv_segment_insert_robust (segment_list_t *list, tcp_seq seq,
                            const uint8_t *data, size_t payload_len,
                            bool_t is_fin)
{
    assert (payload_len > 0 || is_fin);

    // [seq_start, seq_end) 인 상황,,,,
    tcp_seq seg_start = seq;
    tcp_seq seg_end = seq + payload_len + (is_fin ? 1 : 0);


        알고리즘을 짜보자
        - 전제: 리스트는 정렬된 상태를 유지하며, overlapping이 없는 상태로
       유지된다.
        - 리스트를 순회한다
        - cur->end <= seq->start 이면 다음 노드로 이동.
        - cur->end > seq->start

이제 robust insert로 넘어갈건데. 아마 이거 한 함수에 할라면 엄청
복잡해질거임.
- 현재 연결 리스트 기반 구조에서 Overlapping이 일어날 수 있는 패턴 정리하기.
- 정리한 패턴에 대응하는 자연어 레벨 알고리즘 짜기
- C에서 헬퍼함수 선언, 뼈대만 만들고 헬퍼함수를 이용해서 robust insert 구현
- 하위 헬퍼함수 구현
이 순서대로 가면 좋을 듯함.

다시 한 번 강조하자면 robust insert에서 해결하고자 하는건 window전후의
overlap이 아님. 이건 이미 상단에서 처리된 거고, segment가 10~20 30~40 이렇게
버퍼에 있는데 15~25, 혹은 15~35, 이런 식으로 겹치는 경우에 대한 거임.


} */

insert_segment_to_receive_buffer의 의사코드

- 초깃값: seq_start = seq, seq_end = seq_start + payload_len + is_FIN_, pos = seq_start
- 노드를 순회한다. node_start = cur->seq, node_end = cur->seq + cur->payload_len + is_FIN
- while루프는 node 조건으로 돌고, pos < seq_end 조건이 깨지면 탈출하도록.
- 한 노드에 대해, 그 노드에 대해 두 개의 작업을 수행: 앞에 삽입, 겹치는 부분은 자르기
- 앞에 삽입 가능한 부분이 있다면(pos < cur->seq 인 경우 ) 삽입한다. 
삽입 범위는 [pos, min(node_start, seq_end)]
이후 pos = min(node_start, seq_end)로 변경. 여기서 pos < seq_end 조건이 깨지면 탈출..
- 노드와 겹치는 부분이 있다면 (pos < node_end 인 경우) 겹치는 부분을 skip
pos = min(node_end, seq_end)로 변경 후, pos < seq_end 조건이 깨지면 탈출

- 루프가 종료 후에 pos < seq_end면 맨 뒤에 해당 부분을 삽입.
