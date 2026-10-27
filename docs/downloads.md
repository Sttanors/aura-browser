# Загрузки / Downloads

Новая функция в исходниках после v0.4.0-preview.2; установщики этого этапа ещё
не опубликованы.

Ctrl+J открывает общий список загрузок всех обычных окон текущего профиля.
Список действует в текущем запуске Aura. Приватные записи видны только в своём
окне и удаляются из списка при его закрытии; сами скачанные файлы остаются на диске.

У активного файла показываются скорость из CEF и приблизительное оставшееся
время при известном полном размере. Пауза и отмена работают из любого обычного
окна профиля. Кнопка «Повторить» у сбоя/отмены запускает новую загрузку исходного
адреса в том же контейнере или приватном контексте; данные отправленных форм не
повторяются. Если вкладка-источник закрыта, Aura создаёт фоновую вкладку для
исходного контекста. Существующие файлы сохраняются с уникальными именами.
Закрытие окна-источника отменяет его незавершённые загрузки; записи обычных
загрузок остаются в других окнах и позволяют повторить скачивание.

У завершённого файла нажмите **SHA-256**. Вставьте ожидаемые 64 шестнадцатеричных
символа или оставьте поле пустым для вычисления. Aura читает файл порциями в
фоновом файловом потоке; после проверки показывает совпадение/несовпадение и хеш,
который можно скопировать. Если файл удалён или изменился во время чтения,
проверка завершается ошибкой. Результат относится к прочитанному содержимому:
после изменения файла запустите проверку снова.

---

Ctrl+J opens a shared download list for the current profile's normal windows.
The list lasts for the current Aura session. Private entries remain in their
own window and disappear when that window closes; downloaded files stay on disk.

Active entries show CEF's transfer speed and an approximate ETA when total size is
known. Pause, resume and cancel work across normal windows. **Retry** starts a new
download of the original address in the same container/private context; submitted
form data is not replayed. If its source tab has closed, a background tab supplies
the original context. Existing files keep their contents through unique filenames.
Closing the source window cancels its unfinished transfers; normal download
records remain available in the other windows and can be retried.

For a completed file, choose **SHA-256**. Paste an expected 64-character hexadecimal
hash or leave the field empty to calculate it. Aura streams the file on a background
file thread and shows the comparison result and a selectable hash. Missing files
and changes detected during reading cause an error. Run verification again after
modifying the file.
