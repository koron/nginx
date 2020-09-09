# How to compile/configure with gunzip request module

## How to compile

configure 時に `--with-http_gunzip_module` を設定してください

```console
$ ./auto/configure --with-http_gunzip_module
```

他のオプションは任意で追加可能です。

あとは普通にビルドしてインストールしてください。

```console
$ make
$ sudo make install
```

## How to configure

必須の設定:

*   `gunzip_request_body on;` -
    本モジュールを有効化する設定。`location` に書く想定。
*   `proxy_set_header content-encoding '';` -
    `Content-Encoding` ヘッダーを削除する設定。ほぼ必須。

任意の設定:

*   `proxy_http_version 1.1;` -
    バックエンドにkeep-aliveでつなぐのに必要
*   `proxy_set_header connection '';` -
    バックエンドにkeep-aliveでつなぐのに必要
*   `keepalive: 100;` -
    バックエンドにkeep-aliveでつなぐのに必要。
    値は適宜調整が必要

## Benchmark performance

**TL;DR**: 特段の過負荷はなく、想定される理論値を裏切らない

Case |N    |C  |R#/s |CPU Load
-----|----:|--:|----:|-------:
GET  |0.5M |10 |20K  |7~10%
GZ1  |100K |5  |2.0K |15~20%
GZ0  |100K |5  |3.0K |10~15%
RAW0 |100K |5  |1.3K |10~15%
RAW1 |100K |5  |1.3K |10~15%

**Backend**: <https://github.com/koron/httpreqinfo>

**Columns**:

* `Case` - ID of test case
* `N` - Number of total requests
* `C` - Number of clients of concurrent requests
* `R#/s` - Number of completed requests per a second
* `CPU Load` - Load of a CPU core. Benchmark environment have 4 core, and use 1 core for nginx, so 25% is maximum.

**Cases**:

* `GET` - Simple GET request
* `GZ1` - POST 10K gzipped JSON, send it with deflate to backend
* `GZ0` - POST 10K gzipped JSON, send it as is to backend
* `RAW0` / `RAW1` - POST 40K raw JSON send it as is to backend
    *   1 と 0 の違いはgunzip request機能がONになっているかいないかのみ

**Consideration**:

以下の考察では R#/s を主な性能指標としている。

* POST はそもそも負荷が高い (GET vs GZ0, GET vs RAW0)

    ネットワーク負荷による R#/s だけではなく、
    nginxのreverse proxy内のデータコピーによるCPU負荷の上昇も観測できる。

* POST サイズが大きくなると負荷が高くなる (GZ0 vs RAW0, GZ1 vs RAW1, GZ0 vs GZ1)

    POSTのサイズ(実際の流量)の増加に伴うように R#/s が下がっている。

* gunzip によるCPU負荷の軽微な増大が観測された (GZ1 vs GZ0)

    計測で約5%(1コア換算で20%)の上昇が見られた。
    これはgunzip としては妥当な負荷といえる。

* 指標で見ると、gunzipによるサイズ低減の効果はCPU負荷を上回っている (GZ1 vs GZ0 vs RAW0)

    gunzipで流量が減ったことで得られたメリットのほうが、
    CPU負荷が増えるデメリットを上回っている。
