# Исправления начальной заготовки Vulkan Starter App

Этот файл описывает проблемы, обнаруженные до реализации лабораторной работы. Они относятся не к варианту фигуры, а к исходному starter-коду: проект компилировался, но при запуске работал некорректно.

## 1. Что было проверено

Команды из `README.md`:

```bash
cmake --preset debug
cmake --build build-debug --parallel
```

обе завершались успешно. Значит, CMake, C++ compiler, Ninja, Vulkan headers и Vulkan loader были найдены. Проблема возникала не во время сборки, а во время выполнения:

```bash
./build-debug/vulkan-starter-app
```

Это важное различие:

- ошибка конфигурации CMake — зависимости или генератор сборки;
- ошибка компиляции — C++ или GLSL;
- validation error — неправильное использование Vulkan во время работы;
- падение процесса — следствие ошибки выполнения или неопределённого поведения.

## 2. Отдельная проблема доступа к графической сессии

При запуске из изолированного окружения появлялось:

```text
Failed to initialize GLFW
```

Переменная `DISPLAY=:0` существовала, но проверка:

```bash
xdpyinfo
```

возвращала:

```text
unable to open display ":0"
```

Это не ошибка Vulkan-кода и не проблема сборки. Процесс не имел доступа к X-серверу, поэтому GLFW не мог подключиться к графической сессии и создать окно.

Правильная проверка — запускать executable из обычного терминала внутри активной графической сессии:

```bash
cd /home/spr0vay/Documents/Coding/sem3/Cg/lab1
./build-debug/vulkan-starter-app
```

Не следует без необходимости выполнять небезопасные команды наподобие `xhost +`. Если программа работает в обычном пользовательском терминале, менять настройки X-сервера не требуется.

После запуска с доступом к экрану GLFW успешно создавал окно, но обнаруживались настоящие ошибки Vulkan.

## 3. Основная ошибка: отправлялся незаписанный command buffer

Validation layer сообщал правило:

```text
VUID-vkQueueSubmit-pCommandBuffers-00070
```

Смысл сообщения: `vkQueueSubmit` получил command buffer, который не находится в исполняемом состоянии и не содержит записанных команд.

### 3.1 Почему это происходило

Изначально `application::render` ничего не делал:

```cpp
void render(const graphics::internal::FrameData& fd) {
    (void)fd;
}
```

Но `graphics::internal::submitAndPresent()` всё равно отправлял два command buffers:

```cpp
const VkCommandBuffer command_buffers[] = {
    vk_command_buffer,
    vk_imgui_command_buffer,
};
```

`vk_command_buffer` был выделен при инициализации, но никогда не проходил обязательный жизненный цикл:

```text
vkResetCommandBuffer
        ↓
vkBeginCommandBuffer
        ↓
запись команд
        ↓
vkEndCommandBuffer
        ↓
vkQueueSubmit
```

Сам факт выделения command buffer не делает его готовым к отправке.

### 3.2 Исправление

В `application::render` добавлена запись корректного command buffer:

```cpp
void render(const graphics::internal::FrameData& fd) {
    auto& context = graphics::internal::context;

    vkResetCommandBuffer(fd.command_buffer, 0);

    const VkCommandBufferBeginInfo command_buffer_begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(fd.command_buffer, &command_buffer_begin);

    const VkClearValue clear_values[] = {
        { .color = {{ 0.10F, 0.10F, 0.10F, 1.0F }} },
        { .depthStencil = { 1.0F, 0 } },
    };

    const VkRenderPassBeginInfo render_pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = context.render_pass,
        .framebuffer = fd.framebuffer,
        .renderArea = { .extent = context.swapchain_extent },
        .clearValueCount =
            sizeof(clear_values) / sizeof(clear_values[0]),
        .pClearValues = clear_values,
    };

    vkCmdBeginRenderPass(fd.command_buffer, &render_pass_begin,
                         VK_SUBPASS_CONTENTS_INLINE);

    // Команды отрисовки лабораторной добавляются здесь.

    vkCmdEndRenderPass(fd.command_buffer);
    vkEndCommandBuffer(fd.command_buffer);
}
```

Пока геометрии нет, render pass только очищает color и depth attachments. Тем не менее command buffer уже корректно записан и может быть отправлен в очередь.

## 4. Неверный layout swapchain image

Второе сообщение validation layer:

```text
InvalidImageLayout
```

ImGui command buffer ожидал swapchain image в состоянии:

```text
VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
```

но фактическим состоянием было:

```text
VK_IMAGE_LAYOUT_UNDEFINED
```

### 4.1 Что такое image layout

`VkImage` может использоваться для разных операций: как render target, источник копирования, texture или изображение для показа. Layout описывает текущее назначение и внутреннее представление image.

Layout нельзя считать просто меткой в C++-переменной. Переход должен быть частью GPU-команд: через render pass или pipeline barrier.

### 4.2 Почему исправление command buffer решило обе ошибки

Основной render pass настроен так:

```text
initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
```

ImGui render pass ожидает и завершает изображение так:

```text
initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
finalLayout   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
```

После добавления основного render pass переходы образуют правильную цепочку:

```text
UNDEFINED
    ↓ основной render pass
COLOR_ATTACHMENT_OPTIMAL
    ↓ ImGui render pass
PRESENT_SRC_KHR
    ↓ vkQueuePresentKHR
изображение показано в окне
```

Поэтому нельзя было просто удалить `vk_command_buffer` из списка отправки. ImGui pass использует `loadOp = LOAD` и рассчитывает, что основной pass уже подготовил и очистил изображение.

## 5. Ложное сообщение об ошибке показа изображения

В исходной функции `submitAndPresent()` условие выглядело так:

```cpp
VkResult result = vkQueuePresentKHR(context.graphics_queue, &present);
if (result == VK_ERROR_OUT_OF_DATE_KHR ||
    result == VK_SUBOPTIMAL_KHR ||
    vk_swapchain_resize_require) {
    rebuildSwapchain(...);
} else {
    std::cerr << "Failed to present Vulkan swapchain image\n";
}
```

При обычном успехе `result == VK_SUCCESS`. Успех не входит в условие пересоздания swapchain, поэтому выполнялся `else` и программа печатала ошибку, хотя показ прошёл нормально.

Исправленный вариант:

```cpp
VkResult result = vkQueuePresentKHR(context.graphics_queue, &present);
if (result == VK_ERROR_OUT_OF_DATE_KHR ||
    result == VK_SUBOPTIMAL_KHR ||
    vk_swapchain_resize_require) {
    rebuildSwapchain(...);
} else if (result != VK_SUCCESS) {
    std::cerr << "Failed to present Vulkan swapchain image\n";
}
```

Теперь ветви имеют правильный смысл:

- `OUT_OF_DATE`, `SUBOPTIMAL` или resize — пересоздать swapchain;
- `VK_SUCCESS` — ничего не сообщать;
- другая ошибка — вывести сообщение.

## 6. Неверный код завершения программы

В `main()` существовала переменная:

```cpp
int status = EXIT_SUCCESS;
```

При ошибках инициализации ей присваивался `EXIT_FAILURE`, но в конце функция всегда возвращала ноль:

```cpp
return 0;
```

Из-за этого shell, IDE или автоматический тест считали запуск успешным даже после ошибки GLFW, ImGui или Vulkan.

Исправление:

```cpp
return status;
```

Проверить код завершения последней команды можно так:

```bash
./build-debug/vulkan-starter-app
echo $?
```

Ноль означает успех, ненулевое значение — ошибку.

## 7. Почему приложение могло упасть с `SIGSEGV`

Validation layers диагностируют неправильные Vulkan-вызовы, но обычно не отменяют их автоматически. После сообщений об unrecorded command buffer и неправильном image layout драйвер продолжал получать некорректную последовательность команд.

Дальнейшее поведение не определено спецификацией: программа могла показать пустое окно, зависнуть или завершиться с segmentation fault. Поэтому `SIGSEGV` был следствием более ранних validation errors, а не отдельным доказательством ошибки обычного C++-указателя.

Практическое правило: при нескольких сообщениях сначала исправляйте самое первое. Последующие часто являются каскадным следствием.

## 8. Предупреждения при сборке

Во время сборки появлялись предупреждения из файла:

```text
build-debug/_deps/vulkanmemoryallocator-src/include/vk_mem_alloc.h
```

Они относились к неиспользуемым параметрам внутри сторонней библиотеки Vulkan Memory Allocator. Эти предупреждения не мешали компиляции и не вызывали падение starter app.

Отличайте:

- `warning` в `build-debug/_deps/...` — сторонняя зависимость;
- `warning` в `source/...` — собственный код, который следует проверить;
- `error` — сборка не завершена;
- `Validation Error` при запуске — ошибка использования Vulkan API.

## 9. Итоговая последовательность кадра после исправлений

```text
glfwPollEvents
    ↓
ImGui_ImplGlfw_NewFrame
    ↓
ImGui::NewFrame
    ↓
application::update
    ↓
ImGui::Render
    ↓
graphics::prepare
  - ожидание fence
  - получение swapchain image
    ↓
application::render
  - reset/begin command buffer
  - основной render pass
  - end command buffer
    ↓
graphics::submitAndPresent
  - запись ImGui command buffer
  - vkQueueSubmit двух command buffers
  - vkQueuePresentKHR
```

После этих исправлений starter app:

- успешно собирается;
- создаёт окно при наличии доступа к графической сессии;
- не отправляет незаписанный command buffer;
- корректно переводит swapchain image между layouts;
- не печатает ложную ошибку после успешного present;
- возвращает правильный process exit code;
- работает без Vulkan validation errors в базовом цикле.

## 10. Как повторить проверку

Из корня проекта:

```bash
cmake --preset debug
cmake --build build-debug --parallel
./build-debug/vulkan-starter-app
```

Ожидаемый результат до реализации фигуры:

- открывается окно с тёмным очищенным фоном;
- отображается демонстрационное окно ImGui;
- терминал не заполняется validation errors;
- приложение продолжает работать до закрытия окна.

Если из изолированного окружения снова появляется `Failed to initialize GLFW`, повторите запуск в обычном графическом терминале. Это ограничение доступа к display, а не возврат исходной ошибки command buffer.
