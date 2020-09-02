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
