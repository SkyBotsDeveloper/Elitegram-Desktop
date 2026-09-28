# Elitegram Desktop

Telegram, enhanced for Windows.

Elitegram Desktop is a Windows x64 client based on Telegram Desktop, with additional privacy controls, power-user features, and call/audio tools. It is independent software and is not affiliated with or endorsed by Telegram.

## Download

The official Windows package is published on the [releases page](https://github.com/SkyBotsDeveloper/Elitegram-Desktop/releases/latest). Check the release's `SHA256SUMS.txt` against the downloaded archive before extracting it. There is no installer in the first release: extract the ZIP, keep its contents together, and run `Elitegram.exe`.

## Highlights

- **Ghost Mode:** per-account controls for online presence, typing/recording, read status, and story views, with Seen on Reply where applicable. These controls affect client behavior; they are not an anonymity guarantee.
- **Keep Deleted Messages:** a local, view-only archive for eligible ordinary private cloud messages already received by this client. Telegram's normal deletion still proceeds. Protected, view-once, TTL/self-destruct, secret, and unsupported content are not advertised as retainable.
- **Accounts and details:** architecture for up to 100 account slots and a display of Telegram data-center information. Five accounts were manually exercised in earlier testing; 100 simultaneous accounts have not been manually validated.
- **Calls and voice:** Selected Audio, Device Audio, Voice Effects, and No Mic Blink. Results depend on Telegram call conditions, device routes, and Windows audio hardware; No Mic Blink is not a guarantee about every remote indicator.
- **Desktop polish:** an isolated Elitegram profile, native Windows branding, and a compact promo card in the Chats sidebar.

## Build from source

This repository derives from official Telegram Desktop v7.2.9 at commit `fb2e33209517e1a34637d837bfadb3783f2fd59c`. Follow the upstream [Windows build instructions](docs/building-win.md) and initialize the repository's submodules. A custom Telegram client needs its own Telegram API ID and hash; provide them through a local, untracked environment or configure mechanism. Never commit them. The project release number is separate from Telegram Desktop's internal base version.

## Privacy and transparency

Elitegram communicates with Telegram infrastructure as a Telegram client and stores ordinary desktop session data locally. Elitegram settings and the eligible deleted-message archive are also local. Open source makes the implementation inspectable; it is not a security or privacy guarantee. Read [PRIVACY.md](PRIVACY.md) and [SECURITY.md](SECURITY.md) before using sensitive accounts.

## Scope

This first public package targets Windows x64. It is not an official Telegram build. Do not rely on Ghost Mode, No Mic Blink, or Keep Deleted Messages to defeat server-side restrictions or to guarantee privacy against other clients. Keep Deleted Messages is scoped to eligible ordinary one-to-one cloud chats, not every deletion scenario.

## Official links

- [Elitegram Desktop source](https://github.com/SkyBotsDeveloper/Elitegram-Desktop)
- [Elitegram for Android](https://github.com/SkyBotsDeveloper/Elitegram)
- [Telegram channel](https://t.me/aboutelite)
- [Creator on Telegram](https://t.me/iflexsid)
- [Instagram](https://instagram.com/elite.sid)

Created and maintained by Siddhartha Abhimanyu.

## License and upstream

Elitegram Desktop is derived from [Telegram Desktop](https://github.com/telegramdesktop/tdesktop). The original authors' copyright and source headers are retained. The code is distributed under GPLv3 or later with the upstream OpenSSL linking exception described in [LEGAL](LEGAL); the full license is in [LICENSE](LICENSE). See [UPSTREAM.md](UPSTREAM.md) for the verified base and [UPSTREAM_README.md](UPSTREAM_README.md) for upstream project information and third-party acknowledgements. Submodules retain their own licenses.
