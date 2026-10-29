FTL Co-op (прототип) - проверка связи ping/pong
==============================================

Эта сборка проверяет только одно: что две копии FTL могут соединиться по сети.
Игрового процесса в коопе пока нет.

--- Установка (каждому игроку) ---

1. Сделай копию папки FTL (где лежит FTLGame.exe).
2. Откати FTL до 1.6.9:
   https://github.com/FTL-Hyperspace/FTL-Version-Rollback
   (Code -> Download ZIP, скопируй содержимое рядом с FTLGame.exe, запусти rollback.bat)
3. Скопируй Hyperspace.dll и xinput1_4.dll рядом с FTLGame.exe.
4. Установи Hyperspace.ftl через Slipstream Mod Manager
   (положи в папку mods у Slipstream, отметь галочкой, нажми Patch).
5. Запусти FTL и начни любой забег.

--- Проверка ---

Консоль в забеге открывается клавишей \

Хост:     COOP_HOST           (порт 47515 UDP, или COOP_HOST <порт>)
Игрок:    COOP_JOIN <ip хоста> (или COOP_JOIN <ip>:<порт>)
Оба:      COOP_PING           пинг и время ответа
          COOP_STATUS         состояние сессии
          COOP_LEAVE          отключиться

Сообщения видны в левом верхнем углу экрана и в FTL_HS.log.

На одном ПК: запусти FTLGame.exe дважды, в одном COOP_HOST,
в другом COOP_JOIN 127.0.0.1
Через интернет: хост пробрасывает UDP 47515 на роутере, или оба ставят
виртуальную сеть (Radmin VPN, ZeroTier, Tailscale) и подключаются по её IP.
Когда Windows спросит, разреши FTL доступ через брандмауэр.

--- Удаление ---

Удали Hyperspace.dll и xinput1_4.dll, затем удали FTLGame.exe и переименуй
FTLGame_orig.exe обратно в FTLGame.exe.
