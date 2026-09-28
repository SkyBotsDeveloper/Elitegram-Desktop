# Privacy and transparency

Elitegram Desktop is open source, but inspectable code alone cannot guarantee privacy or security.

As a Telegram client, Elitegram communicates with Telegram infrastructure to authenticate, synchronize chats, and use Telegram features. It stores normal Telegram Desktop application and session data locally. Elitegram-specific settings and feature state may also be stored locally under its separate desktop profile.

When enabled, Keep Deleted Messages can retain eligible ordinary private-cloud message content and locally available media in an Elitegram-owned local archive after Telegram's normal deletion. This local copy can persist across app restarts. It is not intended to retain protected, view-once, TTL/self-destruct, or secret-chat ephemeral content. Anyone with access to the local profile or backups may be able to access retained data; protect the device accordingly.

Ghost Mode and related controls alter client behavior but do not make an account anonymous or prevent Telegram, other clients, or other participants from observing all activity. Call/audio controls may use microphone, selected files, and device audio only as configured and permitted by the operating system. Behavior can vary with call state and hardware.

Download only from the [official repository](https://github.com/SkyBotsDeveloper/Elitegram-Desktop), check the published checksum, and report security concerns using [SECURITY.md](SECURITY.md). Elitegram is independent of Telegram.
