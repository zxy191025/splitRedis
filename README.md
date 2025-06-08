## Split Redis version 6.2.6
* Author  :   Jakee Zhao
* Date    :   2025/05

### 1.0 Data Struct
* 2025/05/30 add zmalloc        <zmalloc>
* 2025/05/30 add SDS            <SDS>
* 2025/05/30 add adlist         <adlist>
* 2025/05/30 add dict           <dict>
* 2025/05/30 add skiplist       <skiplist>
* 2025/05/30 add intset         <intset>
* 2025/06/03 add ziplist        <ziplist>
* 2025/06/03 add redisObject    <redisObject>

### 2.0 Implementation of a Single-User Database

#### 2.1 2025/06/05 add redisIO        
 *  A rio object provides the following methods:
 *  read: read from stream.
 *  write: write to stream.
 *  tell: get the current offset.

#### 2.2 2025/06/08 add rdb
 *  RDB persistence method can snapshot and store instance data within a specified time interval (when there are M changes within N seconds), which means full backup.