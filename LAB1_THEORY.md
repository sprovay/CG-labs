# Лабораторная работа №1: теория и руководство по реализации

Этот конспект покрывает всю работу: от математики 3D-графики до структуры Vulkan-кода, интерфейса ImGui, анимации и нескольких объектов. Он написан с учётом текущего проекта `vulkan-starter-app` и не предполагает знаний графического программирования. Достаточно понимать базовый C++: переменные, функции, структуры, условия и циклы.

> Важно: не вставляйте весь код одним блоком. Vulkan-программа состоит из зависимых этапов, и ошибка в раннем этапе проявляется позже. Идите по разделу «План реализации», собирайте проект после каждого шага и сразу исправляйте сообщения validation layers.

## 0. Как пользоваться этим конспектом

Не требуется запоминать весь Vulkan API. Для выполнения лабораторной нужно понять связи между объектами и научиться узнавать одинаковый шаблон во всех вызовах API.

Рекомендуемый порядок чтения:

1. Прочитать разделы 0–4, не программируя. Цель — понять путь вершины до пикселя и смысл матриц.
2. Собрать и запустить исходную заготовку.
3. Прочитать раздел 6 и вручную выписать вершины своего варианта.
4. Реализовывать разделы 7–18 по одному, сверяясь с планом в конце.
5. После появления статичной фигуры добавлять пункты на оценки 4 и 5.

Обозначения фрагментов кода:

- блок с полноценной функцией можно переносить в проект, заменив имена своих переменных;
- короткий блок с `...` показывает только существенную часть и сам по себе не компилируется;
- текстовая формула объясняет идею и не является синтаксисом C++.

### 0.1 Что уже есть в проекте, а что предстоит написать

Заготовка уже умеет:

- открыть окно;
- выбрать видеокарту;
- создать Vulkan device и очередь команд;
- создать swapchain — набор изображений окна;
- создать depth buffer;
- начать и закончить кадр;
- показать интерфейс ImGui.

Вам не нужно писать Vulkan с абсолютного нуля. Ваша часть начинается в основном в `source/application.cpp`:

- описать вершину и состояние приложения;
- создать геометрию;
- создать GPU-буферы;
- создать descriptor set и pipeline;
- обновлять матрицы;
- записать команду рисования;
- уничтожить созданные ресурсы.

Небольшие типы и объявления лучше вынести в `source/application.hpp`, а GLSL-код — в каталог `shaders`.

### 0.2 Минимум C++, который встретится в коде

#### `std::vector`

`std::vector<T>` — изменяемый массив элементов типа `T`:

```cpp
std::vector<uint32_t> indices{0, 1, 2};
indices.push_back(3);       // добавить элемент
indices.size();            // количество элементов
indices.data();            // указатель на непрерывный массив
indices[i];                // элемент с индексом i
```

Vulkan — C API, поэтому обычно получает пару «указатель + количество»:

```cpp
.vertexBindingDescriptionCount =
    static_cast<uint32_t>(bindings.size()),
.pVertexBindingDescriptions = bindings.data(),
```

#### Ссылки, указатели и адрес

```cpp
Mesh mesh;          // объект
Mesh& ref = mesh;   // ссылка: второе имя того же объекта
Mesh* ptr = &mesh;  // указатель: хранит адрес объекта

ref.indexCount = 3;
ptr->indexCount = 3;
```

Выражение `&value` берёт адрес. Vulkan часто просит адрес структуры:

```cpp
vkCreateBuffer(device, &createInfo, nullptr, &buffer);
```

Здесь `&createInfo` — входные параметры, а `&buffer` — место, куда функция запишет созданный handle.

#### Время жизни переменных

Локальная переменная уничтожается при выходе из функции. Vulkan-функции создания обычно копируют описание сразу, поэтому локальный `createInfo` безопасен. Но GPU-ресурсы, на которые ссылается descriptor set или command buffer, должны существовать до завершения работы GPU.

#### `sizeof`, `offsetof` и приведение типов

```cpp
sizeof(Vertex)                    // размер одной вершины в байтах
sizeof(Vertex) * vertices.size()  // размер всего массива
offsetof(Vertex, color)           // смещение поля color внутри Vertex
static_cast<uint32_t>(value)      // явное безопасное преобразование типа
```

#### Namespace

`graphics::internal::context` означает переменную `context` внутри вложенных пространств имён `graphics` и `internal`. Namespace группирует имена и предотвращает конфликты.

#### RAII и почему здесь много ручной очистки

В обычном современном C++ ресурс часто уничтожается деструктором объекта. Чистый Vulkan API использует C-handles, поэтому для каждого успешного `vkCreate...` обычно нужен соответствующий `vkDestroy...`. VMA аналогично сопоставляет `vmaCreateBuffer` и `vmaDestroyBuffer`.

### 0.3 CPU и GPU

CPU выполняет C++-код: обрабатывает интерфейс, вычисляет матрицы, записывает команды. GPU выполняет шейдеры и команды рисования. Они работают асинхронно: вызов `vkQueueSubmit` передаёт работу в очередь, но не ждёт её завершения.

Отсюда три важных правила:

1. Данные сначала должны оказаться в памяти, доступной GPU.
2. Команды записываются в command buffer, а затем отправляются в очередь.
3. Нельзя изменять или уничтожать используемый ресурс, пока GPU не закончил работу.

В starter app синхронизацию упрощает один fence: `prepare()` ждёт завершения предыдущего кадра. Но `application::update()` вызывается **до** `prepare()`, поэтому изменяемые GPU-buffers безопаснее заполнять в `render()`, который вызывается уже после ожидания fence.

### 0.4 Что такое API, handle и структура `CreateInfo`

Vulkan API — набор функций и структур. Почти каждый объект создаётся одним шаблоном:

```cpp
VkSomethingCreateInfo info{
    .sType = VK_STRUCTURE_TYPE_SOMETHING_CREATE_INFO,
    // остальные настройки
};

VkSomething object = VK_NULL_HANDLE;
VkResult result = vkCreateSomething(device, &info, nullptr, &object);
if (result != VK_SUCCESS) {
    // обработать ошибку
}
```

Handle — непрозрачный идентификатор внутреннего объекта драйвера. Нельзя обращаться к его полям; handle только передаётся другим Vulkan-функциям.

Поле `sType` сообщает драйверу точный тип структуры. `pNext = nullptr` означает отсутствие расширений этой структуры. Пользовательский allocator в лабораторной не нужен, поэтому предпоследний аргумент `vkCreate...` обычно `nullptr`.

### 0.5 Единицы измерения и соглашения

- Позиции и расстояния задаются в условных единицах сцены, не в пикселях.
- Цветовые компоненты — числа `float` от `0.0` до `1.0`.
- GLM-функции вращения принимают радианы; интерфейс удобнее показывать в градусах и преобразовывать через `glm::radians`.
- Индексация массивов начинается с нуля.
- Три индекса задают один треугольник.
- Координата точки использует `w = 1`, направление — `w = 0`.

### 0.6 Заголовки, которые понадобятся в `application.cpp`

Точный набор зависит от реализации, но типичная версия начинается так:

```cpp
#include "application.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <imgui.h>
```

Не копируйте include дважды. Компилятор обычно прямо сообщает, если имя неизвестно, и часто подсказывает недостающий заголовок.

Файлы проекта имеют разные роли:

```text
CMakeLists.txt                 сборка и компиляция шейдеров
source/main.cpp                окно и главный цикл
source/graphics_internal.*     инфраструктура Vulkan/swapchain/ImGui
source/application.*           ваша сцена, ресурсы и команды рисования
shaders/object.vert            vertex shader
shaders/object.frag            fragment shader
```

Сначала старайтесь менять только `application.*`, `shaders/*` и необходимые строки CMake. Инфраструктуру swapchain изменяйте лишь при понятной причине.

## 1. Что требуется сделать

Основная часть:

1. Построить сетку выбранного трёхмерного объекта.
2. Передать вершины и индексы в Vulkan-буферы.
3. Создать графический pipeline и шейдеры.
4. Каждый кадр вычислять преобразования объекта и матрицу проекции.
5. Нарисовать объект в реальном времени.

Оценивание:

- На **3**: выбранная фигура без дополнительных заданий.
- На **4**: основная часть и дополнительные задания 1–5.
- На **5**: всё предыдущее и несколько объектов с несколькими `VkDescriptorSet`.

Дополнительные задания:

1. Переключение перспективной и ортографической проекций.
2. Изменение позиции, вращения и масштаба через ImGui.
3. Движение и вращение по сложной траектории; пауза, скорость и параметры траектории.
4. Выбор общего цвета через `ImGui::ColorEdit`.
5. Процедурный цвет каждой вершины, умножаемый на общий цвет.
6. Несколько объектов с отдельными наборами дескрипторов.

Карта задания по разделам:

| Требование | Что изучить |
|---|---|
| Статичная 3D-фигура | Разделы 2–14 |
| Две проекции | Разделы 4, 15 и 16 |
| Position/rotation/scale | Разделы 4 и 15 |
| Анимация | Раздел 16 |
| Выбор цвета | Разделы 11 и 15 |
| Процедурные цвета вершин | Разделы 6.12 и 11 |
| Несколько descriptor sets | Раздел 17 |
| Корректное создание/очистка | Разделы 18–23 |

## 2. Как кадр превращается в изображение

Упрощённый путь данных:

```text
CPU: вершины + индексы
          |
          v
vertex buffer / index buffer в памяти GPU
          |
          v
vertex shader: local -> world -> camera -> clip space
          |
          v
сборка треугольников, отсечение, perspective divide
          |
          v
растеризация в пиксели
          |
          v
fragment shader: вычисление цвета
          |
          v
depth test + запись в swapchain image
          |
          v
ImGui поверх сцены -> показ изображения
```

Vulkan почти ничего не делает неявно. Программист сам создаёт буферы, описывает формат вершин, pipeline, render pass, синхронизацию и команды рисования.

### 2.1 Что делает каждый этап

1. **Vertex shader** запускается отдельно для каждой вершины. Он обязан записать `gl_Position` и может передать цвет дальше.
2. **Primitive assembly** объединяет каждые три индекса в треугольник.
3. **Clipping** обрезает части треугольников за пределами видимой области.
4. **Perspective divide** делит координаты на `w`, создавая эффект перспективы.
5. **Rasterizer** определяет, какие пиксели покрывает треугольник.
6. Значения между вершинами интерполируются. Если красная, зелёная и синяя вершины образуют треугольник, внутри получится плавный градиент.
7. **Fragment shader** запускается для потенциального пикселя и возвращает цвет.
8. **Depth test** сравнивает глубину нового фрагмента с уже сохранённой глубиной.
9. **Color output** записывает прошедший тест цвет в изображение.

Fragment — кандидат в пиксель, а не обязательно окончательный пиксель: его может отбросить depth test.

### 2.2 Почему используется два шейдера

Vertex shader отвечает за положение геометрии. Fragment shader отвечает за внешний вид пикселей. Между ними GPU автоматически интерполирует выходы vertex shader. Имена переменных могут различаться, но их `location` и типы должны совпадать.

### 2.3 Почему ImGui рисуется вторым

Сначала основной render pass очищает экран и рисует 3D-сцену. Затем отдельный pass ImGui загружает уже готовый цвет (`loadOp = LOAD`) и рисует интерфейс поверх. Если очистить изображение во втором pass, 3D-фигура исчезнет за фоном ImGui.

## 3. Геометрия: вершины, треугольники и индексы

### 3.1 Вершина

Для этой лабораторной достаточно позиции и цвета:

```cpp
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
};
```

GPU получает массив `Vertex`. Отдельный массив индексов задаёт, какие тройки вершин образуют треугольники:

```cpp
std::vector<Vertex> vertices;
std::vector<uint32_t> indices;

// Два треугольника квадрата: (0, 1, 2) и (2, 3, 0).
indices = { 0, 1, 2, 2, 3, 0 };
```

Индексный буфер позволяет не копировать одну и ту же вершину для соседних треугольников. Вызов:

```cpp
vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
```

означает: нарисовать `indexCount` индексов, один экземпляр объекта, начиная с индекса 0.

### 3.1.1 Как вручную проверить один треугольник

Перед сложной фигурой полезно нарисовать единственный треугольник:

```cpp
std::vector<Vertex> vertices{
    {{-0.7F, -0.5F, 0.0F}, {1.0F, 0.0F, 0.0F}},
    {{ 0.7F, -0.5F, 0.0F}, {0.0F, 1.0F, 0.0F}},
    {{ 0.0F,  0.7F, 0.0F}, {0.0F, 0.0F, 1.0F}},
};
std::vector<uint32_t> indices{0, 1, 2};
```

Если этот треугольник виден и имеет цветовой градиент, значит pipeline, форматы вершин, buffers, descriptors и шейдеры в основном настроены верно. После этого заменяйте только данные геометрии.

### 3.2 Порядок обхода граней

Три вершины треугольника имеют направление обхода: по или против часовой стрелки. Pipeline может отбрасывать обратные грани (`back-face culling`). В Vulkan направление задаётся полем `frontFace`:

```cpp
.cullMode = VK_CULL_MODE_BACK_BIT,
.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
```

Если часть граней исчезла, проверьте порядок индексов. Для диагностики временно используйте:

```cpp
.cullMode = VK_CULL_MODE_NONE,
```

### 3.3 Глубина

Без depth buffer дальний треугольник может быть нарисован поверх ближнего. Для правильной 3D-картинки нужны:

```cpp
.depthTestEnable = VK_TRUE,
.depthWriteEnable = VK_TRUE,
.depthCompareOp = VK_COMPARE_OP_LESS,
```

Стартовый проект уже создаёт depth image и добавляет его в framebuffer.

### 3.4 Нормали нужны не всегда

Нормаль — единичный вектор, перпендикулярный поверхности. Она нужна для освещения, но в формулировке этой лабораторной освещение отсутствует. Для процедурного цвета по позиции достаточно `position` и `color`. Не добавляйте нормали, освещение и текстуры, пока обязательные пункты не работают.

## 4. Векторы и матрицы

### 4.0 Математический минимум

Вектор `vec3(x, y, z)` можно понимать как:

- точку в пространстве;
- смещение от одной точки до другой;
- направление.

Основные операции:

```cpp
glm::vec3 a{1.0F, 2.0F, 3.0F};
glm::vec3 b{4.0F, 0.0F, 1.0F};

glm::vec3 sum = a + b;
glm::vec3 scaled = 2.0F * a;
float length = glm::length(a);
glm::vec3 direction = glm::normalize(a);
float dot = glm::dot(a, b);
glm::vec3 perpendicular = glm::cross(a, b);
```

Скалярное произведение `dot` показывает близость направлений: для перпендикулярных векторов оно равно нулю. Векторное произведение `cross` даёт перпендикуляр и используется для ориентации граней и камеры.

Нормализация превращает вектор в направление длины 1. Нельзя нормализовать нулевой вектор.

### 4.0.1 Зачем точке четвёртая координата

Обычная 3D-точка имеет `(x, y, z)`, но в графике используется однородная координата `(x, y, z, w)`:

- точка: `w = 1`, поэтому перенос влияет на неё;
- направление: `w = 0`, поэтому перенос не меняет его;
- после проекции `w` хранит величину, необходимую для перспективного деления.

Матрица `mat4` имеет размер 4×4 и может одним умножением представить масштаб, вращение, перенос или проекцию.

### 4.0.2 Интуиция матрицы

Матрица — функция преобразования, записанная числами. Умножение `M * point` означает «применить преобразование M к точке». Умножение матриц объединяет преобразования заранее.

Единичная матрица ничего не меняет:

```cpp
glm::mat4 identity{1.0F};
```

Если итоговая матрица равна `P * V * M`, к вершине первой применяется правая матрица `M`, затем `V`, затем `P`.

### 4.1 Системы координат

Вершина проходит несколько пространств:

1. **Local/model space** — координаты внутри модели, обычно вокруг начала координат.
2. **World space** — положение объекта в общей сцене.
3. **View/camera space** — координаты относительно камеры.
4. **Clip space** — результат матрицы проекции.
5. **NDC** — после деления `x`, `y`, `z` на `w`.
6. **Screen space** — координаты пикселей viewport.

Итоговая формула при столбцовых векторах GLM:

```text
clipPosition = Projection * View * Model * localPosition
```

Порядок важен: матрицы некоммутативны, то есть `A * B` обычно не равно `B * A`.

### 4.2 Model matrix

Перенос, вращение, масштаб и их композиции называются аффинными преобразованиями. Они сохраняют прямые линии и параллельность, но не обязательно длины и углы. Перспективная проекция уже не является аффинным преобразованием: параллельные линии визуально могут сходиться.

Model matrix объединяет перенос, вращение и масштаб:

```cpp
glm::mat4 model{1.0F};
model = glm::translate(model, position);
model = glm::rotate(model, glm::radians(rotationDegrees.x), {1, 0, 0});
model = glm::rotate(model, glm::radians(rotationDegrees.y), {0, 1, 0});
model = glm::rotate(model, glm::radians(rotationDegrees.z), {0, 0, 1});
model = glm::scale(model, scale);
```

При таком порядке точка сначала масштабируется, затем вращается, затем переносится. Если сначала перенести, а потом вращать, объект начнёт вращаться вокруг мирового начала координат.

Не задавайте масштаб равным нулю: матрица станет вырожденной.

Вращение происходит вокруг локального начала координат. Поэтому геометрию удобно изначально центрировать. Если нужен другой pivot, применяют перенос к pivot, вращение и обратный перенос:

```text
Model = Translate(position) * Translate(pivot)
      * Rotate * Translate(-pivot) * Scale
```

### 4.3 View matrix

Камеру удобно задавать через `lookAt`:

```cpp
glm::mat4 view = glm::lookAt(
    glm::vec3(0.0F, 0.0F, 5.0F), // позиция камеры
    glm::vec3(0.0F, 0.0F, 0.0F), // точка, куда она смотрит
    glm::vec3(0.0F, 1.0F, 0.0F)  // направление «вверх»
);
```

View matrix фактически выполняет преобразование, обратное перемещению камеры.

### 4.4 Перспективная проекция

Перспектива делает далёкие объекты визуально меньше. Параметры:

- вертикальный угол обзора `fovY`;
- отношение ширины окна к высоте `aspect`;
- ближняя плоскость `near`;
- дальняя плоскость `far`.

```cpp
float aspect = static_cast<float>(extent.width) /
               static_cast<float>(extent.height);
glm::mat4 projection = glm::perspective(
    glm::radians(fovDegrees), aspect, 0.1F, 100.0F);
```

Чем меньше `near` относительно `far`, тем хуже точность depth buffer. Значения `0.1` и `100` разумны для небольшой сцены; не используйте `near = 0`.

GLM исторически ориентирован на OpenGL, где ось Y clip space отличается от Vulkan. Поэтому после `glm::perspective` обычно делают:

```cpp
projection[1][1] *= -1.0F;
```

Для диапазона глубины Vulkan `[0, 1]` GLM должен видеть настройку:

```cpp
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
```

В этом проекте не вставляйте `#define` вручную, если добавили `GLM_FORCE_DEPTH_ZERO_TO_ONE` через CMake в следующем разделе. Это два альтернативных способа задать одну настройку; CMake-вариант надёжнее.

### 4.5 Ортографическая проекция

При ортографической проекции размер объекта не зависит от расстояния:

```cpp
float halfHeight = orthoSize;
float halfWidth = orthoSize * aspect;
glm::mat4 projection = glm::ortho(
    -halfWidth, halfWidth,
    -halfHeight, halfHeight,
    0.1F, 100.0F);
projection[1][1] *= -1.0F;
```

Переключение проекции меняет только матрицу `projection`; геометрию и pipeline пересоздавать не нужно.

## 5. Подключение GLM

GLM — header-only библиотека линейной алгебры. Её удобно добавить рядом с остальными зависимостями в `CMakeLists.txt`:

```cmake
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)

FetchContent_MakeAvailable(
    glfw
    vk-bootstrap
    VulkanMemoryAllocator
    imgui
    glm
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glm::glm
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)
```

Если системный пакет GLM уже установлен, можно вместо `FetchContent` использовать `find_package(glm CONFIG REQUIRED)`.

После `GLM_FORCE_DEPTH_ZERO_TO_ONE` обычных include достаточно:

```cpp
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
```

Не определяйте `GLM_FORCE_DEPTH_ZERO_TO_ONE` в случайном `.cpp` после подключения заголовков GLM: настройка должна быть одинаковой во всех единицах трансляции, поэтому CMake definition надёжнее.

## 6. Генерация вариантов фигур

Все фигуры удобнее центрировать около `(0, 0, 0)` и делать размером примерно 1–2 единицы. Тогда одна и та же камера подходит для всех вариантов.

Полезные helpers уменьшают ошибки индексов:

```cpp
void addTriangle(std::vector<uint32_t>& indices,
                 uint32_t a, uint32_t b, uint32_t c) {
    indices.push_back(a);
    indices.push_back(b);
    indices.push_back(c);
}

void addQuad(std::vector<uint32_t>& indices,
             uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    // Четырёхугольник a-b-c-d превращается в два треугольника.
    addTriangle(indices, a, b, c);
    addTriangle(indices, c, d, a);
}
```

Условия корректной сетки:

- каждый индекс меньше `vertices.size()`;
- `indices.size()` кратен трём;
- соседние треугольники не имеют случайно противоположного обхода;
- у замкнутой фигуры нет незапланированных дыр;
- вершины не содержат `NaN` после деления или нормализации.

### 6.0.1 Как триангулировать многоугольник

GPU в этой работе рисует треугольники. Выпуклый многоугольник с вершинами `v0, v1, ..., vn` можно разбить веером:

```text
(v0, v1, v2), (v0, v2, v3), ..., (v0, v[n-1], vn)
```

У пятиугольника будет 3 треугольника, у шестиугольника — 4. Для невыпуклого многоугольника простой веер может выйти за его границы, но все грани фигур этой лабораторной выпуклы.

### 6.0.2 Как строится параметрическая сетка

Сфера и тор задаются двумя параметрами. Пусть в каждом ряду `columns` вершин. Четыре угла одной ячейки:

```cpp
uint32_t a = row * columns + column;
uint32_t b = a + 1;
uint32_t c = (row + 1) * columns + column + 1;
uint32_t d = (row + 1) * columns + column;
addQuad(indices, a, b, c, d);
```

На замкнутом направлении либо добавляют дублированный последний ряд/столбец, либо соединяют индексы через остаток `%`. Дублирование проще для новичка.

### 6.1 Куб / правильный гексаэдр

Куб имеет координаты всех комбинаций `x, y, z = ±a`. Это 8 уникальных позиций и 12 треугольников. Гексаэдр — другое название куба.

Если цвет должен различаться между гранями, вершины нельзя полностью разделять между гранями: понадобится 24 вершины, по 4 на каждую грань. Для процедурного цвета по позиции достаточно 8 вершин.

Полный пример данных куба с полуразмером `0.5`:

```cpp
const std::vector<glm::vec3> positions{
    {-0.5F, -0.5F, -0.5F}, // 0
    { 0.5F, -0.5F, -0.5F}, // 1
    { 0.5F,  0.5F, -0.5F}, // 2
    {-0.5F,  0.5F, -0.5F}, // 3
    {-0.5F, -0.5F,  0.5F}, // 4
    { 0.5F, -0.5F,  0.5F}, // 5
    { 0.5F,  0.5F,  0.5F}, // 6
    {-0.5F,  0.5F,  0.5F}, // 7
};

std::vector<Vertex> vertices;
vertices.reserve(positions.size());
for (const glm::vec3& p : positions) {
    // Функция proceduralColor приведена в разделе 6.12.
    vertices.push_back({p, proceduralColor(p, 0.5F)});
}

const std::vector<uint32_t> indices{
    4, 5, 6,  6, 7, 4, // передняя грань +Z
    1, 0, 3,  3, 2, 1, // задняя грань -Z
    0, 4, 7,  7, 3, 0, // левая грань -X
    5, 1, 2,  2, 6, 5, // правая грань +X
    3, 7, 6,  6, 2, 3, // верхняя грань +Y
    0, 1, 5,  5, 4, 0, // нижняя грань -Y
};
```

На первом запуске отключите culling. Если куб виден, затем включите `BACK_BIT`. Если внешние грани исчезли, поменяйте `frontFace` или разверните порядок каждой тройки индексов. Это зависит от соглашения проекции и viewport.

### 6.2 Параллелепипед

Это куб с разными полуосями:

```text
x = ±width/2, y = ±height/2, z = ±depth/2
```

Топология индексов совпадает с кубом.

### 6.3 Пирамида

Для квадратного основания:

```text
основание: (-a,0,-a), (a,0,-a), (a,0,a), (-a,0,a)
вершина:   (0,h,0)
```

Основание — два треугольника, боковые стороны — ещё четыре.

### 6.4 Октаэдр

Шесть вершин лежат на осях:

```text
(±a,0,0), (0,±a,0), (0,0,±a)
```

Восемь треугольных граней соединяют верхнюю и нижнюю вершины с четырьмя вершинами «экватора».

### 6.5 Икосаэдр

Используется золотое сечение:

```cpp
float phi = (1.0F + std::sqrt(5.0F)) / 2.0F;
```

Двенадцать вершин получаются перестановками:

```text
(0, ±1, ±phi), (±1, ±phi, 0), (±phi, 0, ±1)
```

После генерации каждую позицию полезно нормализовать и умножить на требуемый радиус. Икосаэдр имеет 20 треугольных граней.

Готовая нумерация и индексы:

```cpp
const float p = (1.0F + std::sqrt(5.0F)) / 2.0F;
std::vector<glm::vec3> positions{
    {-1, p, 0}, {1, p, 0}, {-1, -p, 0}, {1, -p, 0},
    {0, -1, p}, {0, 1, p}, {0, -1, -p}, {0, 1, -p},
    {p, 0, -1}, {p, 0, 1}, {-p, 0, -1}, {-p, 0, 1},
};
for (glm::vec3& position : positions) {
    position = glm::normalize(position) * radius;
}

std::vector<uint32_t> indices{
    0,11,5,  0,5,1,   0,1,7,   0,7,10,  0,10,11,
    1,5,9,   5,11,4,  11,10,2, 10,7,6,  7,1,8,
    3,9,4,   3,4,2,   3,2,6,   3,6,8,   3,8,9,
    4,9,5,   2,4,11,  6,2,10,  8,6,7,   9,8,1,
};
```

Затем преобразуйте `positions` в `vertices`, вычисляя цвет каждой позиции.

### 6.6 Додекаэдр

Додекаэдр двойственен икосаэдру. Надёжный способ построения:

1. Создать икосаэдр.
2. Для каждой его треугольной грани вычислить центр и нормализовать — это вершина додекаэдра.
3. Для каждой вершины икосаэдра собрать центры пяти соседних граней.
4. Отсортировать их по углу вокруг исходной вершины — получится пятиугольная грань.
5. Разбить пятиугольник веером на три треугольника.

Это безопаснее, чем вручную поддерживать большой список индексов.

Чтобы выполнить шаг 3, переберите 20 граней икосаэдра и для каждой из трёх исходных вершин запомните индекс центра этой грани. Для сортировки пяти центров вокруг исходной вершины `n` постройте локальные оси:

```cpp
glm::vec3 n = glm::normalize(icosahedronVertex);
glm::vec3 helper = std::abs(n.y) < 0.9F
    ? glm::vec3(0, 1, 0)
    : glm::vec3(1, 0, 0);
glm::vec3 axisX = glm::normalize(glm::cross(helper, n));
glm::vec3 axisY = glm::cross(n, axisX);
```

Для каждого центра `c` вычислите:

```cpp
glm::vec3 direction = glm::normalize(c);
float angle = std::atan2(glm::dot(direction, axisY),
                         glm::dot(direction, axisX));
```

Отсортируйте пары `(angle, index)` по возрастанию угла. Если после включения culling грань смотрит внутрь, разверните порядок пяти индексов.

### 6.7 Усечённый правильный тетраэдр

Сначала создаётся правильный тетраэдр, например с вершинами:

```text
(1,1,1), (1,-1,-1), (-1,1,-1), (-1,-1,1)
```

На каждом ориентированном ребре `A -> B` создаётся точка:

```text
P = A + (B - A) / 3
```

Получается 12 вершин. Около старых вершин возникают 4 треугольника, а каждая из 4 старых граней превращается в шестиугольник. Шестиугольник триангулируется веером или четырьмя треугольниками без добавления центра.

### 6.8 Сфера примерно на 100 вершин

UV-сфера строится кольцами:

```cpp
for (uint32_t y = 0; y <= stacks; ++y) {
    float v = static_cast<float>(y) / stacks;
    float theta = v * glm::pi<float>();

    for (uint32_t x = 0; x <= sectors; ++x) {
        float u = static_cast<float>(x) / sectors;
        float phi = u * glm::two_pi<float>();

        glm::vec3 p{
            radius * std::sin(theta) * std::cos(phi),
            radius * std::cos(theta),
            radius * std::sin(theta) * std::sin(phi)
        };
        vertices.push_back({p, proceduralColor(p)});
    }
}
```

Для каждой ячейки между двумя кольцами создаются два треугольника. Например, `stacks = 8`, `sectors = 12` даёт 117 записей с дублированным швом. Полюса и шов можно оптимизировать, если требуется около 100 уникальных вершин.

### 6.9 Цилиндр примерно с 50 вершинами в основаниих

Для `segments = 50`:

```cpp
float angle = 2.0F * glm::pi<float>() * i / segments;
float x = radius * std::cos(angle);
float z = radius * std::sin(angle);
```

Создайте верхнее и нижнее кольцо, затем боковые четырёхугольники разбейте на два треугольника. Для крышек добавьте две центральные вершины и треугольные веера. Следите, чтобы верхняя и нижняя крышки имели противоположный порядок обхода.

### 6.10 Конус

Конус похож на цилиндр, но верхнее кольцо заменяется одной вершиной `(0, height/2, 0)`. Основание строится треугольным веером.

### 6.11 Тор

Параметрическая формула с большим радиусом `R` и радиусом трубки `r`:

```text
x = (R + r cos(v)) cos(u)
y = r sin(v)
z = (R + r cos(v)) sin(u)
```

`u` идёт вокруг центра тора, `v` — вокруг сечения трубки. Оба угла находятся в `[0, 2π]`. Сетка строится как у сферы: каждая параметрическая ячейка превращается в два треугольника.

### 6.12 Процедурный цвет вершины

Если координаты лежат около диапазона `[-radius, radius]`, их можно перевести в `[0, 1]`:

```cpp
glm::vec3 proceduralColor(const glm::vec3& p, float radius = 1.0F) {
    return glm::clamp(p / (2.0F * radius) + glm::vec3(0.5F),
                      glm::vec3(0.0F), glm::vec3(1.0F));
}
```

Для сферы можно использовать нормализованное направление, а для тора — параметры `u` и `v`. Главное, чтобы цвет был вычислен из локальных данных вершины, а не был одинаковым для всей модели.

### 6.13 Законченные генераторы фигур

Предыдущие подразделы объясняют математику. Ниже приведён единый практический каркас, чтобы формулы можно было превратить в работающий C++ без скрытых шагов.

Общий тип результата и helper для выпуклой грани:

```cpp
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

void addFaceOutward(const std::vector<glm::vec3>& positions,
                    std::vector<uint32_t> face,
                    std::vector<uint32_t>& indices) {
    if (face.size() < 3) {
        return;
    }

    glm::vec3 center{0.0F};
    for (uint32_t index : face) {
        center += positions[index];
    }
    center /= static_cast<float>(face.size());

    glm::vec3 normal = glm::cross(
        positions[face[1]] - positions[face[0]],
        positions[face[2]] - positions[face[0]]);

    // Для выпуклой фигуры с центром в начале координат нормаль
    // внешней грани направлена примерно туда же, куда её центр.
    if (glm::dot(normal, center) < 0.0F) {
        std::reverse(face.begin(), face.end());
    }

    for (size_t i = 1; i + 1 < face.size(); ++i) {
        indices.push_back(face[0]);
        indices.push_back(face[i]);
        indices.push_back(face[i + 1]);
    }
}

MeshData finishMesh(const std::vector<glm::vec3>& positions,
                    std::vector<uint32_t> indices) {
    float bound = 0.0F;
    for (const glm::vec3& p : positions) {
        bound = std::max(bound, std::abs(p.x));
        bound = std::max(bound, std::abs(p.y));
        bound = std::max(bound, std::abs(p.z));
    }
    bound = std::max(bound, 0.0001F);

    MeshData result;
    result.vertices.reserve(positions.size());
    for (const glm::vec3& p : positions) {
        result.vertices.push_back({p, proceduralColor(p, bound)});
    }
    result.indices = std::move(indices);
    return result;
}
```

Для `std::reverse` нужен `<algorithm>`, для `std::move` — `<utility>`.

#### Куб, гексаэдр и параллелепипед

```cpp
MeshData makeBox(float width, float height, float depth) {
    const float x = width * 0.5F;
    const float y = height * 0.5F;
    const float z = depth * 0.5F;
    const std::vector<glm::vec3> p{
        {-x,-y,-z}, {x,-y,-z}, {x,y,-z}, {-x,y,-z},
        {-x,-y, z}, {x,-y, z}, {x,y, z}, {-x,y, z},
    };
    std::vector<uint32_t> indices;
    for (std::vector<uint32_t> face : {
             std::vector<uint32_t>{4,5,6,7}, {0,1,2,3},
             {0,4,7,3}, {1,5,6,2},
             {3,7,6,2}, {0,4,5,1}}) {
        addFaceOutward(p, std::move(face), indices);
    }
    return finishMesh(p, std::move(indices));
}

// Куб / правильный гексаэдр:
MeshData cube = makeBox(1.0F, 1.0F, 1.0F);
// Параллелепипед:
MeshData box = makeBox(1.6F, 1.0F, 0.7F);
```

#### Квадратная пирамида

```cpp
MeshData makePyramid(float halfBase, float height) {
    const std::vector<glm::vec3> p{
        {-halfBase, 0, -halfBase},
        { halfBase, 0, -halfBase},
        { halfBase, 0,  halfBase},
        {-halfBase, 0,  halfBase},
        {0, height, 0},
    };
    std::vector<uint32_t> indices;
    for (std::vector<uint32_t> face : {
             std::vector<uint32_t>{0,1,2,3},
             {0,1,4}, {1,2,4}, {2,3,4}, {3,0,4}}) {
        addFaceOutward(p, std::move(face), indices);
    }
    return finishMesh(p, std::move(indices));
}
```

Центр этой пирамиды не совпадает с началом координат по Y, поэтому автоматическая проверка направления нижней грани может быть неоднозначна. При отключённом culling изображение корректно; для culling можно после генерации сдвинуть все позиции на `-height/4` по Y или вручную проверить основание.

#### Правильный октаэдр

```cpp
MeshData makeOctahedron(float radius) {
    const std::vector<glm::vec3> p{
        {0, radius, 0}, {0, -radius, 0},
        {radius, 0, 0}, {0, 0, radius},
        {-radius, 0, 0}, {0, 0, -radius},
    };
    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t a = 2 + i;
        const uint32_t b = 2 + (i + 1) % 4;
        addFaceOutward(p, {0, a, b}, indices);
        addFaceOutward(p, {1, a, b}, indices);
    }
    return finishMesh(p, std::move(indices));
}
```

#### Усечённый правильный тетраэдр

```cpp
MeshData makeTruncatedTetrahedron(float radius) {
    std::array<glm::vec3, 4> original{
        glm::vec3{ 1, 1, 1}, glm::vec3{ 1,-1,-1},
        glm::vec3{-1, 1,-1}, glm::vec3{-1,-1, 1},
    };
    for (glm::vec3& p : original) {
        p = glm::normalize(p) * radius;
    }

    std::vector<glm::vec3> positions;
    std::array<std::array<uint32_t, 4>, 4> edgeIndex{};
    for (uint32_t from = 0; from < 4; ++from) {
        for (uint32_t to = 0; to < 4; ++to) {
            if (from == to) continue;
            edgeIndex[from][to] =
                static_cast<uint32_t>(positions.size());
            positions.push_back(original[from]
                + (original[to] - original[from]) / 3.0F);
        }
    }

    std::vector<uint32_t> indices;
    // Четыре треугольника около исходных вершин.
    for (uint32_t center = 0; center < 4; ++center) {
        std::vector<uint32_t> face;
        for (uint32_t other = 0; other < 4; ++other) {
            if (other != center) face.push_back(edgeIndex[center][other]);
        }
        addFaceOutward(positions, std::move(face), indices);
    }

    // Четыре исходные грани становятся шестиугольниками.
    const std::array<std::array<uint32_t, 3>, 4> faces{{
        {0,1,2}, {0,3,1}, {0,2,3}, {1,3,2},
    }};
    for (const auto& f : faces) {
        const uint32_t a = f[0], b = f[1], c = f[2];
        addFaceOutward(positions,
            {edgeIndex[a][b], edgeIndex[b][a],
             edgeIndex[b][c], edgeIndex[c][b],
             edgeIndex[c][a], edgeIndex[a][c]}, indices);
    }
    return finishMesh(positions, std::move(indices));
}
```

#### UV-сфера без дублированных полюсов

При `rings = 9` и `segments = 10` получается `2 + 9 × 10 = 92` вершины — достаточно близко к требованию «примерно 100».

```cpp
MeshData makeSphere(float radius,
                    uint32_t rings = 9,
                    uint32_t segments = 10) {
    std::vector<glm::vec3> p;
    p.push_back({0, radius, 0}); // верхний полюс, индекс 0

    for (uint32_t ring = 1; ring <= rings; ++ring) {
        const float theta = glm::pi<float>() * ring / (rings + 1);
        for (uint32_t segment = 0; segment < segments; ++segment) {
            const float phi = glm::two_pi<float>() * segment / segments;
            p.push_back({
                radius * std::sin(theta) * std::cos(phi),
                radius * std::cos(theta),
                radius * std::sin(theta) * std::sin(phi),
            });
        }
    }
    const uint32_t bottom = static_cast<uint32_t>(p.size());
    p.push_back({0, -radius, 0});

    std::vector<uint32_t> indices;
    for (uint32_t s = 0; s < segments; ++s) {
        const uint32_t next = (s + 1) % segments;
        addFaceOutward(p, {0, 1 + s, 1 + next}, indices);
    }
    for (uint32_t ring = 0; ring + 1 < rings; ++ring) {
        const uint32_t first = 1 + ring * segments;
        const uint32_t second = first + segments;
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t next = (s + 1) % segments;
            addFaceOutward(p,
                {first + s, first + next,
                 second + next, second + s}, indices);
        }
    }
    const uint32_t lastRing = 1 + (rings - 1) * segments;
    for (uint32_t s = 0; s < segments; ++s) {
        const uint32_t next = (s + 1) % segments;
        addFaceOutward(p,
            {bottom, lastRing + next, lastRing + s}, indices);
    }
    return finishMesh(p, std::move(indices));
}
```

Проверяйте `rings >= 1`, `segments >= 3` до вычислений.

#### Цилиндр

```cpp
MeshData makeCylinder(float radius, float height,
                      uint32_t segments = 50) {
    std::vector<glm::vec3> p;
    const float half = height * 0.5F;
    for (uint32_t i = 0; i < segments; ++i) {
        const float a = glm::two_pi<float>() * i / segments;
        p.push_back({radius * std::cos(a), -half,
                     radius * std::sin(a)});
    }
    for (uint32_t i = 0; i < segments; ++i) {
        const float a = glm::two_pi<float>() * i / segments;
        p.push_back({radius * std::cos(a), half,
                     radius * std::sin(a)});
    }
    const uint32_t bottomCenter = static_cast<uint32_t>(p.size());
    p.push_back({0, -half, 0});
    const uint32_t topCenter = static_cast<uint32_t>(p.size());
    p.push_back({0, half, 0});

    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < segments; ++i) {
        const uint32_t next = (i + 1) % segments;
        addFaceOutward(p, {i, next, segments + next, segments + i},
                       indices);
        addFaceOutward(p, {bottomCenter, i, next}, indices);
        addFaceOutward(p, {topCenter, segments + i, segments + next},
                       indices);
    }
    return finishMesh(p, std::move(indices));
}
```

#### Конус

```cpp
MeshData makeCone(float radius, float height,
                  uint32_t segments = 50) {
    std::vector<glm::vec3> p;
    const float half = height * 0.5F;
    for (uint32_t i = 0; i < segments; ++i) {
        const float a = glm::two_pi<float>() * i / segments;
        p.push_back({radius * std::cos(a), -half,
                     radius * std::sin(a)});
    }
    const uint32_t apex = static_cast<uint32_t>(p.size());
    p.push_back({0, half, 0});
    const uint32_t center = static_cast<uint32_t>(p.size());
    p.push_back({0, -half, 0});

    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < segments; ++i) {
        const uint32_t next = (i + 1) % segments;
        addFaceOutward(p, {apex, i, next}, indices);
        addFaceOutward(p, {center, i, next}, indices);
    }
    return finishMesh(p, std::move(indices));
}
```

#### Тор

```cpp
MeshData makeTorus(float majorRadius, float minorRadius,
                   uint32_t majorSegments = 24,
                   uint32_t minorSegments = 12) {
    std::vector<glm::vec3> p;
    for (uint32_t i = 0; i < majorSegments; ++i) {
        const float u = glm::two_pi<float>() * i / majorSegments;
        for (uint32_t j = 0; j < minorSegments; ++j) {
            const float v = glm::two_pi<float>() * j / minorSegments;
            const float ring = majorRadius + minorRadius * std::cos(v);
            p.push_back({ring * std::cos(u),
                         minorRadius * std::sin(v),
                         ring * std::sin(u)});
        }
    }

    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < majorSegments; ++i) {
        const uint32_t nextI = (i + 1) % majorSegments;
        for (uint32_t j = 0; j < minorSegments; ++j) {
            const uint32_t nextJ = (j + 1) % minorSegments;
            const uint32_t a = i * minorSegments + j;
            const uint32_t b = nextI * minorSegments + j;
            const uint32_t c = nextI * minorSegments + nextJ;
            const uint32_t d = i * minorSegments + nextJ;
            // Порядок a-d-c-b соответствует внешней нормали тора.
            addTriangle(indices, a, d, c);
            addTriangle(indices, c, b, a);
        }
    }
    return finishMesh(p, std::move(indices));
}
```

Проверяйте `majorRadius > minorRadius > 0`, `majorSegments >= 3`, `minorSegments >= 3`.

#### Икосаэдр

Самостоятельная функция на основе данных раздела 6.5:

```cpp
MeshData makeIcosahedron(float radius) {
    const float phi = (1.0F + std::sqrt(5.0F)) / 2.0F;
    std::vector<glm::vec3> p{
        {-1,phi,0}, {1,phi,0}, {-1,-phi,0}, {1,-phi,0},
        {0,-1,phi}, {0,1,phi}, {0,-1,-phi}, {0,1,-phi},
        {phi,0,-1}, {phi,0,1}, {-phi,0,-1}, {-phi,0,1},
    };
    for (glm::vec3& position : p) {
        position = glm::normalize(position) * radius;
    }
    std::vector<uint32_t> indices{
        0,11,5,  0,5,1,   0,1,7,   0,7,10,  0,10,11,
        1,5,9,   5,11,4,  11,10,2, 10,7,6,  7,1,8,
        3,9,4,   3,4,2,   3,2,6,   3,6,8,   3,8,9,
        4,9,5,   2,4,11,  6,2,10,  8,6,7,   9,8,1,
    };
    return finishMesh(p, std::move(indices));
}
```

#### Додекаэдр

Ниже алгоритм из раздела 6.6 собран в самостоятельную функцию.

```cpp
MeshData makeDodecahedron(float radius) {
    const float phi = (1.0F + std::sqrt(5.0F)) / 2.0F;
    const std::vector<glm::vec3> icosaPositions{
        {-1,phi,0}, {1,phi,0}, {-1,-phi,0}, {1,-phi,0},
        {0,-1,phi}, {0,1,phi}, {0,-1,-phi}, {0,1,-phi},
        {phi,0,-1}, {phi,0,1}, {-phi,0,-1}, {-phi,0,1},
    };
    const std::vector<uint32_t> icosaIndices{
        0,11,5,  0,5,1,   0,1,7,   0,7,10,  0,10,11,
        1,5,9,   5,11,4,  11,10,2, 10,7,6,  7,1,8,
        3,9,4,   3,4,2,   3,2,6,   3,6,8,   3,8,9,
        4,9,5,   2,4,11,  6,2,10,  8,6,7,   9,8,1,
    };

    std::vector<glm::vec3> centers;
    std::array<std::vector<uint32_t>, 12> incident;

    for (size_t f = 0; f < icosaIndices.size(); f += 3) {
        const uint32_t a = icosaIndices[f];
        const uint32_t b = icosaIndices[f + 1];
        const uint32_t c = icosaIndices[f + 2];
        const glm::vec3 center = glm::normalize(
            icosaPositions[a] + icosaPositions[b] + icosaPositions[c])
            * radius;
        const uint32_t centerIndex =
            static_cast<uint32_t>(centers.size());
        centers.push_back(center);
        incident[a].push_back(centerIndex);
        incident[b].push_back(centerIndex);
        incident[c].push_back(centerIndex);
    }

    std::vector<uint32_t> indices;
    for (uint32_t vertex = 0; vertex < 12; ++vertex) {
        const glm::vec3 n = glm::normalize(icosaPositions[vertex]);
        const glm::vec3 helper = std::abs(n.y) < 0.9F
            ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 axisX = glm::normalize(glm::cross(helper, n));
        const glm::vec3 axisY = glm::cross(n, axisX);

        auto& face = incident[vertex];
        std::sort(face.begin(), face.end(),
            [&](uint32_t lhs, uint32_t rhs) {
                auto angle = [&](uint32_t index) {
                    const glm::vec3 d = glm::normalize(centers[index]);
                    return std::atan2(glm::dot(d, axisY),
                                      glm::dot(d, axisX));
                };
                return angle(lhs) < angle(rhs);
            });
        addFaceOutward(centers, face, indices);
    }
    return finishMesh(centers, std::move(indices));
}
```

Абсолютный размер исходного икосаэдра не важен: при вычислении центра используется `normalize`.

### 6.14 Как выбрать генератор варианта

Одна функция возвращает одинаковый `MeshData`, поэтому остальная Vulkan-часть не зависит от фигуры:

```cpp
MeshData createChosenFigure() {
    // Оставьте ровно один return для своего варианта.
    return makeSphere(1.0F);
    // return makeBox(1.0F, 1.0F, 1.0F);
    // return makeOctahedron(1.0F);
    // return makePyramid(0.8F, 1.5F);
    // return makeTruncatedTetrahedron(1.0F);
    // return makeCylinder(0.7F, 1.5F);
    // return makeCone(0.8F, 1.5F);
    // return makeTorus(1.0F, 0.35F);
    // return makeIcosahedron(1.0F);
    // return makeDodecahedron(1.0F);
}
```

После генерации полезно проверить:

```cpp
MeshData data = createChosenFigure();
std::cout << "vertices: " << data.vertices.size()
          << ", triangles: " << data.indices.size() / 3 << '\n';
```

## 7. Ресурсы Vulkan, необходимые приложению

### 7.0 Словарь объектов Vulkan простыми словами

| Объект | Смысл |
|---|---|
| `VkInstance` | Связь приложения с Vulkan runtime и расширениями. |
| `VkPhysicalDevice` | Физическая видеокарта и её возможности. |
| `VkDevice` | Логическое подключение приложения к видеокарте. |
| `VkQueue` | Очередь работ для GPU. |
| `VkSurfaceKHR` | Связь Vulkan с окном GLFW. |
| `VkSwapchainKHR` | Набор изображений, по очереди показываемых в окне. |
| `VkImage` | Двумерные/трёхмерные данные изображения на GPU. |
| `VkImageView` | Описание того, как использовать конкретное image. |
| `VkRenderPass` | Правила использования color/depth attachments. |
| `VkFramebuffer` | Конкретные image views, подставленные в render pass. |
| `VkBuffer` | Линейные байты для вершин, индексов или uniform. |
| `VkPipeline` | Шейдеры и фиксированные настройки рисования. |
| `VkDescriptorSet` | Ссылки на ресурсы, доступные шейдерам. |
| `VkCommandBuffer` | Записанный список команд для GPU. |
| `VkSemaphore` | Синхронизация операций GPU между собой. |
| `VkFence` | Сигнал, который CPU может ждать от GPU. |

Не путайте `VkRenderPass` и `VkFramebuffer`: render pass задаёт общие правила и форматы, framebuffer подставляет конкретные изображения текущего swapchain.

### 7.0.1 Один кадр по шагам в starter app

1. `glfwPollEvents` получает события окна.
2. `application::update` строит UI и готовит CPU-копию uniform-данных.
3. `graphics::internal::prepare` ждёт fence предыдущего кадра.
4. `vkAcquireNextImageKHR` получает индекс свободного swapchain image.
5. `application::render` записывает команды 3D-сцены.
6. `submitAndPresent` записывает команды ImGui.
7. `vkQueueSubmit` отправляет оба command buffers на GPU.
8. `vkQueuePresentKHR` просит показать готовое изображение.

Semaphore `image_available` гарантирует, что рисование не начнётся до получения swapchain image. Semaphore `image_finished` гарантирует, что показ не начнётся до завершения рисования. Fence не позволяет CPU перезаписать command buffer предыдущего кадра слишком рано.

Помимо ресурсов стартового проекта понадобятся:

```cpp
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct Mesh {
    Buffer vertexBuffer;
    Buffer indexBuffer;
    uint32_t indexCount = 0;
};

VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;
VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
Buffer uniformBuffer;
void* mappedUniformData = nullptr;
UniformData pendingUniformData{};
```

Для первой версии допустимо хранить эти переменные в безымянном namespace внутри `application.cpp`:

```cpp
namespace application {
namespace {

Mesh mesh;
Buffer uniformBuffer;
void* mappedUniformData = nullptr;
UniformData pendingUniformData{};
VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;
Controls controls;

} // namespace

bool initialize() { /* создание */ }
void update(double time) { /* UI и матрицы */ }
void render(const graphics::internal::FrameData& fd) { /* команды */ }
void shutdown() { /* очистка */ }
} // namespace application
```

Безымянный namespace делает имена доступными только этому `.cpp`. Это простой учебный вариант. Когда код вырастет, ресурсы можно собрать в класс, но для лабораторной это необязательно.

Полезный helper для проверки Vulkan-вызовов:

```cpp
bool checkVk(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) {
        return true;
    }
    std::cerr << operation << " failed, VkResult = " << result << '\n';
    return false;
}
```

Не все успешные результаты равны только `VK_SUCCESS`: например, swapchain отдельно обрабатывает `VK_SUBOPTIMAL_KHR`. Для обычных `vkCreate...` проверка выше подходит.

Различайте понятия:

- `VkBuffer` — описание линейного диапазона данных;
- `VmaAllocation` — выделенная память VMA для этого буфера;
- `VkDescriptorSetLayout` — схема ресурсов, ожидаемых шейдером;
- `VkDescriptorSet` — конкретные буферы/изображения, подставленные в схему;
- `VkPipelineLayout` — объединяет descriptor layouts и push constants;
- `VkPipeline` — почти всё фиксированное состояние отрисовки и шейдеры.

## 8. Uniform buffer и выравнивание

Данные, меняющиеся каждый кадр, удобно передавать через uniform buffer:

```cpp
struct alignas(16) UniformData {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 projection;
    alignas(16) glm::vec4 tint;
};
```

`vec4` используется вместо `vec3`, потому что правила выравнивания uniform block (`std140`) выравнивают трёхкомпонентный вектор как четырёхкомпонентный. `mat4` занимает 64 байта.

Подготовка новых данных на CPU:

```cpp
UniformData ubo{
    .model = model,
    .view = view,
    .projection = projection,
    .tint = glm::vec4(selectedColor, 1.0F),
};
pendingUniformData = ubo;
```

Сам `memcpy` в mapped buffer выполняйте в начале `application::render`, после того как `prepare()` дождался fence:

```cpp
std::memcpy(mappedUniformData,
            &pendingUniformData,
            sizeof(pendingUniformData));
vmaFlushAllocation(graphics::internal::context.allocator,
                   uniformBuffer.allocation,
                   0, sizeof(pendingUniformData));
```

Сам uniform buffer создаётся один раз в `initialize`:

```cpp
if (!createBuffer(sizeof(UniformData),
                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  uniformBuffer,
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
    return false;
}

VmaAllocationInfo allocationInfo{};
vmaGetAllocationInfo(context.allocator,
                     uniformBuffer.allocation,
                     &allocationInfo);
mappedUniformData = allocationInfo.pMappedData;
if (mappedUniformData == nullptr) {
    std::cerr << "Uniform buffer is not mapped\n";
    return false;
}
```

`memcpy` требует `#include <cstring>`. Копируется именно `sizeof(UniformData)`, а не размер указателя.

В текущем starter app одновременно обрабатывается один кадр, потому что используется один fence и один основной command buffer. Поэтому одного uniform buffer достаточно. При переходе к нескольким frames in flight нужны отдельные изменяемые ресурсы для каждого кадра.

## 9. Создание буферов через VMA

Создать `VkBuffer` недостаточно: ему ещё нужна память подходящего типа. Одни типы памяти быстро читаются GPU, другие доступны CPU. Ручной выбор и распределение памяти в Vulkan многословны, поэтому проект использует Vulkan Memory Allocator (VMA).

Mapped memory — участок GPU allocation, которому соответствует обычный CPU-указатель. Запись через `memcpy` меняет содержимое buffer. Флаг `HOST_ACCESS_SEQUENTIAL_WRITE` сообщает VMA, что CPU будет последовательно записывать данные; `MAPPED` просит сразу оставить allocation отображённым.

Память бывает host-coherent и non-coherent. В первом случае записи CPU видны GPU автоматически, во втором нужен flush. `vmaFlushAllocation` безопасно вызывать в обоих случаях: VMA выполнит необходимое действие либо ничего не сделает. Поэтому учебные helpers вызывают flush после каждого завершённого `memcpy`.

Учебный простой вариант — host-visible память:

```cpp
bool createBuffer(VkDeviceSize size,
                  VkBufferUsageFlags usage,
                  Buffer& result,
                  VmaAllocationCreateFlags flags = 0) {
    VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo allocationInfo{
        .flags = flags,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };

    return vmaCreateBuffer(graphics::internal::context.allocator,
                           &bufferInfo, &allocationInfo,
                           &result.handle, &result.allocation,
                           nullptr) == VK_SUCCESS;
}
```

Создание и заполнение vertex buffer:

```cpp
createBuffer(sizeof(Vertex) * vertices.size(),
             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
             mesh.vertexBuffer,
             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
             VMA_ALLOCATION_CREATE_MAPPED_BIT);

VmaAllocationInfo info{};
vmaGetAllocationInfo(context.allocator,
                     mesh.vertexBuffer.allocation, &info);
std::memcpy(info.pMappedData, vertices.data(),
            sizeof(Vertex) * vertices.size());
vmaFlushAllocation(context.allocator, mesh.vertexBuffer.allocation,
                   0, VK_WHOLE_SIZE);
```

Индексный буфер создаётся тем же способом:

```cpp
createBuffer(sizeof(uint32_t) * indices.size(),
             VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
             mesh.indexBuffer,
             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
             VMA_ALLOCATION_CREATE_MAPPED_BIT);

VmaAllocationInfo indexInfo{};
vmaGetAllocationInfo(context.allocator,
                     mesh.indexBuffer.allocation, &indexInfo);
std::memcpy(indexInfo.pMappedData, indices.data(),
            sizeof(uint32_t) * indices.size());
vmaFlushAllocation(context.allocator, mesh.indexBuffer.allocation,
                   0, VK_WHOLE_SIZE);

mesh.indexCount = static_cast<uint32_t>(indices.size());
```

Перед копированием обязательно проверьте, что vectors не пусты, создание buffer вернуло успех, а `pMappedData != nullptr`. Не вызывайте `vertices.data()` как источник ненулевого копирования для пустого массива.

### 9.1 Законченные helpers загрузки mesh

Чтобы не дублировать проверку, используйте шаблонную функцию. Шаблон здесь означает, что один и тот же код работает для `Vertex` и `uint32_t`:

```cpp
template <typename T>
bool createAndUploadBuffer(const std::vector<T>& source,
                           VkBufferUsageFlags usage,
                           Buffer& destination) {
    if (source.empty()) {
        std::cerr << "Cannot create an empty buffer\n";
        return false;
    }
    const VkDeviceSize byteSize = sizeof(T) * source.size();
    if (!createBuffer(byteSize, usage, destination,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
        return false;
    }

    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(graphics::internal::context.allocator,
                         destination.allocation, &allocationInfo);
    if (allocationInfo.pMappedData == nullptr) {
        vmaDestroyBuffer(graphics::internal::context.allocator,
                         destination.handle, destination.allocation);
        destination = {};
        return false;
    }
    std::memcpy(allocationInfo.pMappedData, source.data(), byteSize);
    vmaFlushAllocation(graphics::internal::context.allocator,
                       destination.allocation, 0, byteSize);
    return true;
}

bool createMeshBuffers(const std::vector<Vertex>& vertices,
                       const std::vector<uint32_t>& indices) {
    if (!createAndUploadBuffer(vertices,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               mesh.vertexBuffer)) {
        return false;
    }
    if (!createAndUploadBuffer(indices,
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               mesh.indexBuffer)) {
        auto& context = graphics::internal::context;
        vmaDestroyBuffer(context.allocator,
                         mesh.vertexBuffer.handle,
                         mesh.vertexBuffer.allocation);
        mesh.vertexBuffer = {};
        return false;
    }
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    return true;
}

void destroyMeshBuffers() {
    auto& context = graphics::internal::context;
    if (mesh.indexBuffer.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator,
                         mesh.indexBuffer.handle,
                         mesh.indexBuffer.allocation);
    }
    if (mesh.vertexBuffer.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator,
                         mesh.vertexBuffer.handle,
                         mesh.vertexBuffer.allocation);
    }
    mesh = {};
}
```

`destination = {}` обнуляет оба поля структуры `Buffer`, чтобы cleanup не попытался повторно уничтожить уже удалённый ресурс.

Для статической геометрии производительнее использовать staging buffer и копирование в device-local buffer с флагом `VK_BUFFER_USAGE_TRANSFER_DST_BIT`. Для одной небольшой лабораторной host-visible вариант проще и обычно достаточен.

Освобождение:

```cpp
vmaDestroyBuffer(context.allocator,
                 buffer.handle, buffer.allocation);
```

Нельзя уничтожать ресурс, пока GPU ещё использует его. Перед общей очисткой starter app вызывает `vkQueueWaitIdle`.

## 10. Descriptor set

Descriptor set можно представить как таблицу аргументов для shader:

```text
set 0
  binding 0 -> uniform buffer с матрицами и цветом
```

Три похожих объекта выполняют разные задачи:

- **layout** описывает тип таблицы: в каком binding какой ресурс ожидается;
- **pool** выделяет память для таблиц;
- **set** — конкретный экземпляр таблицы со ссылкой на конкретный buffer.

Pipeline layout сообщает pipeline, какие descriptor set layouts будут привязаны. GLSL-объявление `layout(set = 0, binding = 0)` должно совпасть с C++ layout. Сам descriptor set можно менять между draw calls, не создавая новый pipeline.

### 10.1 Layout

Vertex shader ожидает uniform buffer в `set = 0, binding = 0`:

```cpp
VkDescriptorSetLayoutBinding binding{
    .binding = 0,
    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
};

VkDescriptorSetLayoutCreateInfo info{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 1,
    .pBindings = &binding,
};
vkCreateDescriptorSetLayout(context.device, &info, nullptr,
                            &descriptorSetLayout);
```

### 10.2 Pool, allocation и update

```cpp
VkDescriptorPoolSize poolSize{
    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .descriptorCount = 1,
};

VkDescriptorPoolCreateInfo poolInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1,
    .poolSizeCount = 1,
    .pPoolSizes = &poolSize,
};
vkCreateDescriptorPool(context.device, &poolInfo, nullptr, &descriptorPool);

VkDescriptorSetAllocateInfo allocateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptorPool,
    .descriptorSetCount = 1,
    .pSetLayouts = &descriptorSetLayout,
};
vkAllocateDescriptorSets(context.device, &allocateInfo, &descriptorSet);

VkDescriptorBufferInfo bufferInfo{
    .buffer = uniformBuffer.handle,
    .offset = 0,
    .range = sizeof(UniformData),
};

VkWriteDescriptorSet write{
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet = descriptorSet,
    .dstBinding = 0,
    .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .pBufferInfo = &bufferInfo,
};
vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
```

`vkUpdateDescriptorSets` записывает в descriptor set ссылку на buffer; сами матрицы обновляются через mapped memory.

### 10.3 Полная функция создания descriptor-ресурсов

Разрозненные шаги выше можно собрать так:

```cpp
void destroyDescriptorResources() {
    auto& context = graphics::internal::context;
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context.device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE;
        descriptorSet = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context.device,
                                     descriptorSetLayout, nullptr);
        descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (uniformBuffer.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator,
                         uniformBuffer.handle, uniformBuffer.allocation);
        uniformBuffer = {};
        mappedUniformData = nullptr;
    }
}

bool createDescriptorResources() {
    auto& context = graphics::internal::context;

    if (!createBuffer(sizeof(UniformData),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      uniformBuffer,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
        return false;
    }
    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(context.allocator,
                         uniformBuffer.allocation, &allocationInfo);
    mappedUniformData = allocationInfo.pMappedData;
    if (mappedUniformData == nullptr) {
        destroyDescriptorResources();
        return false;
    }

    const VkDescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    if (vkCreateDescriptorSetLayout(context.device, &layoutInfo, nullptr,
                                    &descriptorSetLayout) != VK_SUCCESS) {
        destroyDescriptorResources();
        return false;
    }

    const VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    if (vkCreateDescriptorPool(context.device, &poolInfo, nullptr,
                               &descriptorPool) != VK_SUCCESS) {
        destroyDescriptorResources();
        return false;
    }

    const VkDescriptorSetAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptorSetLayout,
    };
    if (vkAllocateDescriptorSets(context.device, &allocateInfo,
                                 &descriptorSet) != VK_SUCCESS) {
        destroyDescriptorResources();
        return false;
    }

    const VkDescriptorBufferInfo bufferInfo{
        .buffer = uniformBuffer.handle,
        .offset = 0,
        .range = sizeof(UniformData),
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &bufferInfo,
    };
    vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
    return true;
}
```

Эта функция очищает свои частично созданные ресурсы при ошибке. Такой шаблон особенно важен в Vulkan: один логический этап часто состоит из нескольких handles.

## 11. Шейдеры GLSL

GLSL похож на C, но выполняется на GPU. Файлы `.vert` и `.frag` не подключаются через `#include` в C++; они отдельно компилируются в SPIR-V, а затем загружаются Vulkan-приложением.

Минимальный синтаксис:

- `#version 450` — версия языка;
- `vec3`/`vec4` — векторы из 3/4 чисел `float`;
- `mat4` — матрица 4×4;
- `in` — вход этапа;
- `out` — выход этапа;
- `uniform` — небольшие данные, одинаковые для всех вершин одного draw call;
- `layout(location = N)` — номер канала между pipeline/шейдерами;
- `.rgb`, `.x`, `.xyz` — выбор компонентов вектора;
- `void main()` — точка входа shader.

Shaders не могут напрямую читать произвольные C++-переменные. Данные становятся доступными только через явно описанные inputs, descriptors, push constants и другие GPU-механизмы.

Создайте `shaders/object.vert`:

```glsl
#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(set = 0, binding = 0) uniform UniformData {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 tint;
} ubo;

layout(location = 0) out vec3 vertexColor;

void main() {
    gl_Position = ubo.projection * ubo.view * ubo.model
                * vec4(inPosition, 1.0);
    vertexColor = inColor * ubo.tint.rgb;
}
```

Создайте `shaders/object.frag`:

```glsl
#version 450

layout(location = 0) in vec3 vertexColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vertexColor, 1.0);
}
```

В `CMakeLists.txt` раскомментируйте/добавьте:

```cmake
compile_shader(object.vert)
compile_shader(object.frag)
```

После конфигурации и сборки появятся `shaders/object.vert.spv` и `shaders/object.frag.spv`. GLSL компилируется в SPIR-V утилитой `glslc`.

Если `glslc` не найден, текущий CMake молча пропустит компиляцию. Проверка:

```bash
command -v glslc
```

### 11.1 Чтение SPIR-V и создание shader module

Pipeline принимает не имя файла, а `VkShaderModule`. Сначала прочитайте бинарный `.spv`:

```cpp
#include <fstream>
#include <stdexcept>

std::vector<uint32_t> readSpirv(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error(std::string("Cannot open shader: ") + path);
    }

    const std::streamsize byteSize = file.tellg();
    if (byteSize <= 0 || byteSize % 4 != 0) {
        throw std::runtime_error("Invalid SPIR-V file size");
    }

    std::vector<uint32_t> code(
        static_cast<size_t>(byteSize) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), byteSize);
    if (!file) {
        throw std::runtime_error("Cannot read SPIR-V file");
    }
    return code;
}

VkShaderModule createShaderModule(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * sizeof(uint32_t),
        .pCode = code.data(),
    };

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(graphics::internal::context.device,
                             &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Cannot create shader module");
    }
    return module;
}
```

Использование:

```cpp
auto vertexCode = readSpirv("shaders/object.vert.spv");
auto fragmentCode = readSpirv("shaders/object.frag.spv");
VkShaderModule vertexModule = createShaderModule(vertexCode);
VkShaderModule fragmentModule = createShaderModule(fragmentCode);
```

Путь относительный, поэтому важна рабочая директория процесса, а не расположение исполняемого файла.

### 11.2 Как сопоставляются C++ и GLSL

| C++ | GLSL | Связь |
|---|---|---|
| `Vertex::position` | `inPosition` | `location = 0` и `R32G32B32_SFLOAT` |
| `Vertex::color` | `inColor` | `location = 1` и `R32G32B32_SFLOAT` |
| `UniformData` buffer | uniform block `ubo` | `set = 0, binding = 0` |
| `descriptorSetLayout` | объявления `set/binding` | типы и shader stage должны совпадать |

Если хоть одна сторона не совпадает, программа может собраться, но validation layer сообщит об ошибке pipeline или данные в shader будут неверными.

## 12. Формат входных вершин

Pipeline должен знать структуру `Vertex`:

```cpp
VkVertexInputBindingDescription vertexBinding{
    .binding = 0,
    .stride = sizeof(Vertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

std::array<VkVertexInputAttributeDescription, 2> attributes{{
    {
        .location = 0,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(Vertex, position),
    },
    {
        .location = 1,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(Vertex, color),
    },
}};
```

`location` обязан совпадать с `layout(location = ...)` в vertex shader.

## 13. Графический pipeline

Pipeline — заранее собранная конфигурация конвейера. Драйверу известны шейдеры, формат вершин, способ сборки треугольников, depth test и смешивание цветов. Это позволяет не передавать десятки независимых настроек перед каждым треугольником.

Pipeline не содержит конкретные вершины и матрицы. Vertex/index buffers и descriptor sets привязываются командами отдельно, поэтому одним pipeline можно рисовать много объектов.

Pipeline включает:

- два shader module;
- vertex input;
- topology (`TRIANGLE_LIST`);
- viewport/scissor;
- rasterizer;
- multisampling;
- depth test;
- color blending;
- pipeline layout;
- совместимый render pass.

Ключевые структуры:

```cpp
VkPipelineInputAssemblyStateCreateInfo inputAssembly{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .primitiveRestartEnable = VK_FALSE,
};

VkPipelineRasterizationStateCreateInfo rasterizer{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .depthClampEnable = VK_FALSE,
    .rasterizerDiscardEnable = VK_FALSE,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_BACK_BIT,
    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
    .depthBiasEnable = VK_FALSE,
    .lineWidth = 1.0F,
};

VkPipelineDepthStencilStateCreateInfo depthStencil{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    .depthTestEnable = VK_TRUE,
    .depthWriteEnable = VK_TRUE,
    .depthCompareOp = VK_COMPARE_OP_LESS,
};

VkPipelineColorBlendAttachmentState colorBlendAttachment{
    .blendEnable = VK_FALSE,
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                      VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT |
                      VK_COLOR_COMPONENT_A_BIT,
};
```

Размер окна меняется, поэтому viewport и scissor удобнее сделать dynamic state:

```cpp
std::array<VkDynamicState, 2> dynamicStates{
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
};
```

Pipeline layout создаётся с `descriptorSetLayout`:

```cpp
VkPipelineLayoutCreateInfo layoutInfo{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &descriptorSetLayout,
};
vkCreatePipelineLayout(context.device, &layoutInfo, nullptr,
                       &pipelineLayout);
```

Затем все состояния передаются в `VkGraphicsPipelineCreateInfo`, где обязательно указать:

```cpp
.layout = pipelineLayout,
.renderPass = context.render_pass,
.subpass = 0,
```

Создание:

```cpp
vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE,
                          1, &pipelineInfo, nullptr, &pipeline);
```

Shader modules после успешного создания pipeline можно уничтожить: pipeline уже содержит нужный код.

### 13.1 Как собрать перечисленные состояния вместе

Ниже показана полная форма функции. Она предполагает, что глобальные `descriptorSetLayout`, `pipelineLayout` и `pipeline` уже объявлены, а helpers из раздела 11 доступны. Для первого запуска установлен `VK_CULL_MODE_NONE`.

```cpp
bool createGraphicsPipeline() {
    auto& context = graphics::internal::context;

    const auto vertexCode = readSpirv("shaders/object.vert.spv");
    const auto fragmentCode = readSpirv("shaders/object.frag.spv");
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;
    try {
        vertexModule = createShaderModule(vertexCode);
        fragmentModule = createShaderModule(fragmentCode);
    } catch (...) {
        if (vertexModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(context.device, vertexModule, nullptr);
        }
        throw;
    }

    const std::array shaderStages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertexModule,
            .pName = "main",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragmentModule,
            .pName = "main",
        },
    };

    const VkVertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array<VkVertexInputAttributeDescription, 2> attributes{{
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, position),
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, color),
        },
    }};

    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount =
            static_cast<uint32_t>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    // Сами viewport/scissor будут заданы командами каждый кадр.
    const VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    const VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .lineWidth = 1.0F,
    };

    const VkPipelineMultisampleStateCreateInfo multisampling{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
    };

    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType =
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    const VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                          VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo colorBlending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };

    const std::array dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount =
            static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };

    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorSetLayout,
    };
    if (vkCreatePipelineLayout(context.device, &layoutInfo, nullptr,
                               &pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(context.device, fragmentModule, nullptr);
        vkDestroyShaderModule(context.device, vertexModule, nullptr);
        return false;
    }

    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = static_cast<uint32_t>(shaderStages.size()),
        .pStages = shaderStages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicState,
        .layout = pipelineLayout,
        .renderPass = context.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    const VkResult result = vkCreateGraphicsPipelines(
        context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

    vkDestroyShaderModule(context.device, fragmentModule, nullptr);
    vkDestroyShaderModule(context.device, vertexModule, nullptr);

    if (result != VK_SUCCESS) {
        vkDestroyPipelineLayout(context.device, pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
        return false;
    }
    return true;
}
```

Почему локальные `CreateInfo` безопасны: `vkCreateGraphicsPipelines` читает их во время вызова. После возврата pipeline существует независимо от этих C++-переменных. Shader modules также больше не нужны после успешного создания pipeline.

Функции `readSpirv`/`createShaderModule` выше используют исключения. Поэтому `application::initialize` должен либо позволить исключению завершить программу с понятным сообщением, либо обернуть вызов в `try/catch`:

```cpp
try {
    if (!createGraphicsPipeline()) {
        std::cerr << "Failed to create graphics pipeline\n";
        return false;
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return false;
}
```

## 14. Запись команд рисования

Текущий `application::render` уже начинает command buffer и render pass. Команды объекта нужно вставить между `vkCmdBeginRenderPass` и `vkCmdEndRenderPass`:

```cpp
VkViewport viewport{
    .x = 0.0F,
    .y = 0.0F,
    .width = static_cast<float>(context.swapchain_extent.width),
    .height = static_cast<float>(context.swapchain_extent.height),
    .minDepth = 0.0F,
    .maxDepth = 1.0F,
};
VkRect2D scissor{ .extent = context.swapchain_extent };

vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);
vkCmdBindPipeline(fd.command_buffer,
                  VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

VkDeviceSize offset = 0;
vkCmdBindVertexBuffers(fd.command_buffer, 0, 1,
                       &mesh.vertexBuffer.handle, &offset);
vkCmdBindIndexBuffer(fd.command_buffer, mesh.indexBuffer.handle,
                     0, VK_INDEX_TYPE_UINT32);
vkCmdBindDescriptorSets(fd.command_buffer,
                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipelineLayout, 0, 1,
                        &descriptorSet, 0, nullptr);
vkCmdDrawIndexed(fd.command_buffer, mesh.indexCount, 1, 0, 0, 0);
```

Тип индексов в `vkCmdBindIndexBuffer` обязан совпадать с C++-типом: `uint32_t` соответствует `VK_INDEX_TYPE_UINT32`.

### 14.1 Полная функция `application::render`

В контексте текущего проекта функция может выглядеть так:

```cpp
void render(const graphics::internal::FrameData& fd) {
    auto& context = graphics::internal::context;

    // prepare() уже дождался завершения предыдущего кадра,
    // поэтому теперь uniform buffer безопасно изменять.
    std::memcpy(mappedUniformData,
                &pendingUniformData,
                sizeof(pendingUniformData));
    vmaFlushAllocation(context.allocator, uniformBuffer.allocation,
                       0, sizeof(pendingUniformData));

    vkResetCommandBuffer(fd.command_buffer, 0);

    const VkCommandBufferBeginInfo commandBufferBegin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(fd.command_buffer, &commandBufferBegin);

    const VkClearValue clearValues[] = {
        {.color = {{0.10F, 0.10F, 0.10F, 1.0F}}},
        {.depthStencil = {1.0F, 0}},
    };
    const VkRenderPassBeginInfo renderPassBegin{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = context.render_pass,
        .framebuffer = fd.framebuffer,
        .renderArea = {
            .offset = {0, 0},
            .extent = context.swapchain_extent,
        },
        .clearValueCount =
            static_cast<uint32_t>(std::size(clearValues)),
        .pClearValues = clearValues,
    };
    vkCmdBeginRenderPass(fd.command_buffer, &renderPassBegin,
                         VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<float>(context.swapchain_extent.width),
        .height = static_cast<float>(context.swapchain_extent.height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    const VkRect2D scissor{
        .offset = {0, 0},
        .extent = context.swapchain_extent,
    };
    vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);

    vkCmdBindPipeline(fd.command_buffer,
                      VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(fd.command_buffer, 0, 1,
                           &mesh.vertexBuffer.handle, &vertexOffset);
    vkCmdBindIndexBuffer(fd.command_buffer, mesh.indexBuffer.handle,
                         0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(fd.command_buffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1,
                            &descriptorSet, 0, nullptr);
    vkCmdDrawIndexed(fd.command_buffer, mesh.indexCount,
                     1, 0, 0, 0);

    vkCmdEndRenderPass(fd.command_buffer);
    vkEndCommandBuffer(fd.command_buffer);
}
```

`std::size` объявлен в `<iterator>` или доступен через некоторые контейнерные заголовки; для полной определённости добавьте `#include <iterator>`. В текущем коде можно оставить выражение `sizeof(clearValues) / sizeof(clearValues[0])`.

Эта функция только записывает команды. Реальное выполнение начинается позже в `vkQueueSubmit`, который уже вызывается инфраструктурой проекта.

В production-коде результаты `vkBeginCommandBuffer` и `vkEndCommandBuffer` тоже проверяют. Но текущая функция возвращает `void`, и простой `return` при ошибке привёл бы к отправке незаписанного command buffer. Для полноценной обработки ошибок нужно изменить интерфейс `render`/главный цикл так, чтобы кадр не отправлялся после ошибки.

## 15. ImGui и состояние приложения

Состояние интерфейса удобно хранить отдельно:

```cpp
enum class ProjectionType { Perspective, Orthographic };

struct Controls {
    glm::vec3 position{0.0F};
    glm::vec3 rotationDegrees{0.0F};
    glm::vec3 scale{1.0F};
    glm::vec3 tint{1.0F};
    ProjectionType projection = ProjectionType::Perspective;
    float fovDegrees = 60.0F;
    float orthoSize = 3.0F;
    bool animationPlaying = false;
    float animationSpeed = 1.0F;
    float trajectoryRadius = 1.5F;
};
```

Интерфейс удобно вынести в функцию:

```cpp
void drawControls() {
    ImGui::Begin("Object controls");
    ImGui::DragFloat3("Position", &controls.position.x, 0.01F);
    ImGui::DragFloat3("Rotation", &controls.rotationDegrees.x, 1.0F);
    ImGui::DragFloat3("Scale", &controls.scale.x,
                      0.01F, 0.01F, 10.0F);
    ImGui::ColorEdit3("Tint", &controls.tint.x);

    int projection = static_cast<int>(controls.projection);
    ImGui::RadioButton("Perspective", &projection,
                       static_cast<int>(ProjectionType::Perspective));
    ImGui::SameLine();
    ImGui::RadioButton("Orthographic", &projection,
                       static_cast<int>(ProjectionType::Orthographic));
    controls.projection = static_cast<ProjectionType>(projection);

    if (controls.projection == ProjectionType::Perspective) {
        ImGui::SliderFloat("FOV", &controls.fovDegrees, 20.0F, 120.0F);
    } else {
        ImGui::SliderFloat("Ortho size", &controls.orthoSize,
                           0.5F, 10.0F);
    }

    ImGui::Checkbox("Play animation", &controls.animationPlaying);
    ImGui::SliderFloat("Speed", &controls.animationSpeed, 0.0F, 5.0F);
    ImGui::SliderFloat("Trajectory radius", &controls.trajectoryRadius,
                       0.0F, 5.0F);
    ImGui::End();
}
```

`DragFloat3` получает адрес первого компонента. Это корректно для `glm::vec3`, компоненты которого хранятся последовательно.

## 16. Анимация и время

Нельзя прибавлять фиксированное значение «за кадр»: скорость тогда зависит от FPS. Используйте время:

```cpp
double previousTime = 0.0;
double animationTime = 0.0;
```

Пример сложной траектории Лиссажу:

```cpp
float t = static_cast<float>(animationTime);
glm::vec3 animatedOffset{
    controls.trajectoryRadius * std::sin(1.0F * t),
    0.5F * controls.trajectoryRadius * std::sin(2.0F * t),
    controls.trajectoryRadius * std::cos(1.5F * t),
};

glm::vec3 animatedRotation = controls.rotationDegrees
    + glm::vec3(35.0F, 70.0F, 20.0F) * t;
```

Итоговая позиция может быть `controls.position + animatedOffset`. При паузе `animationTime` не меняется, поэтому объект не прыгает после продолжения.

### 16.1 Полное вычисление uniform-данных одного объекта

Эта функция соединяет управление, анимацию и матрицы:

```cpp
UniformData makeUniformData() {
    const auto& extent = graphics::internal::context.swapchain_extent;
    const float safeHeight = static_cast<float>(
        std::max(extent.height, uint32_t{1}));
    const float aspect = static_cast<float>(extent.width) / safeHeight;

    const float t = static_cast<float>(animationTime);
    glm::vec3 position = controls.position;
    glm::vec3 rotation = controls.rotationDegrees;

    // animationPlaying управляет только ростом animationTime.
    // Смещение применяется и на паузе, поэтому объект не прыгает назад.
    position += glm::vec3{
        controls.trajectoryRadius * std::sin(t),
        controls.trajectoryRadius * 0.5F * std::sin(2.0F * t),
        controls.trajectoryRadius * 0.5F * std::cos(1.5F * t),
    };
    rotation += glm::vec3(35.0F, 70.0F, 20.0F) * t;

    glm::mat4 model{1.0F};
    model = glm::translate(model, position);
    model = glm::rotate(model, glm::radians(rotation.x), {1, 0, 0});
    model = glm::rotate(model, glm::radians(rotation.y), {0, 1, 0});
    model = glm::rotate(model, glm::radians(rotation.z), {0, 0, 1});
    model = glm::scale(model, controls.scale);

    const glm::mat4 view = glm::lookAt(
        glm::vec3(0.0F, 0.0F, 5.0F),
        glm::vec3(0.0F),
        glm::vec3(0.0F, 1.0F, 0.0F));

    glm::mat4 projection{1.0F};
    if (controls.projection == ProjectionType::Perspective) {
        projection = glm::perspective(
            glm::radians(controls.fovDegrees), aspect, 0.1F, 100.0F);
    } else {
        const float halfHeight = controls.orthoSize;
        const float halfWidth = halfHeight * aspect;
        projection = glm::ortho(-halfWidth, halfWidth,
                                -halfHeight, halfHeight,
                                0.1F, 100.0F);
    }
    projection[1][1] *= -1.0F;

    return {
        .model = model,
        .view = view,
        .projection = projection,
        .tint = glm::vec4(controls.tint, 1.0F),
    };
}
```

Для `std::max` нужен `<algorithm>`. После построения UI сохраните CPU-копию:

```cpp
pendingUniformData = makeUniformData();
```

Здесь размер окна берётся из swapchain, поэтому пропорции автоматически обновляются после resize.

### 16.2 Полная функция `application::update`

Теперь можно соединить UI, время и uniform buffer:

```cpp
void update(double currentTime) {
    double deltaTime = previousTime == 0.0
        ? 0.0
        : currentTime - previousTime;
    previousTime = currentTime;

    // После остановки в debugger следующий delta может быть огромным.
    deltaTime = std::min(deltaTime, 0.1);

    drawControls();

    if (controls.animationPlaying) {
        animationTime += deltaTime * controls.animationSpeed;
    }

    // Пока сохраняем только CPU-копию. В GPU-buffer она попадёт
    // в render(), после ожидания fence.
    pendingUniformData = makeUniformData();
}
```

`main.cpp` уже вызывает `ImGui::NewFrame()` перед `application::update`, поэтому создавать ImGui-окно здесь правильно. `ImGui::Render()` вызывается позже, после `update`.

## 17. Несколько объектов и несколько VkDescriptorSet

Для оценки 5 удобно разделить общую сетку и состояние экземпляра:

```cpp
struct SceneObject {
    glm::vec3 position{0.0F};
    glm::vec3 rotation{0.0F};
    glm::vec3 scale{1.0F};
    glm::vec3 tint{1.0F};
    Buffer uniformBuffer;
    void* mappedUniformData = nullptr;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

std::vector<SceneObject> objects;
```

Один `descriptorSetLayout`, один pipeline и одна mesh могут использоваться всеми объектами. Но каждый объект получает собственные:

- uniform buffer с `model` и `tint`;
- `VkDescriptorSet`, ссылающийся на этот buffer.

Pool должен быть рассчитан на количество объектов:

```cpp
VkDescriptorPoolSize poolSize{
    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .descriptorCount = objectCount,
};

VkDescriptorPoolCreateInfo poolInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = objectCount,
    .poolSizeCount = 1,
    .pPoolSizes = &poolSize,
};
```

После создания pool выделите сразу нужное число sets. Vulkan требует массив layouts той же длины:

```cpp
std::vector<VkDescriptorSetLayout> layouts(objectCount,
                                            descriptorSetLayout);
std::vector<VkDescriptorSet> sets(objectCount, VK_NULL_HANDLE);

VkDescriptorSetAllocateInfo allocateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptorPool,
    .descriptorSetCount = objectCount,
    .pSetLayouts = layouts.data(),
};
if (vkAllocateDescriptorSets(context.device, &allocateInfo,
                             sets.data()) != VK_SUCCESS) {
    return false;
}

for (uint32_t i = 0; i < objectCount; ++i) {
    objects[i].descriptorSet = sets[i];

    VkDescriptorBufferInfo bufferInfo{
        .buffer = objects[i].uniformBuffer.handle,
        .offset = 0,
        .range = sizeof(UniformData),
    };
    VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = objects[i].descriptorSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &bufferInfo,
    };
    vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
}
```

До этого цикла у каждого `objects[i]` уже должен быть создан собственный `uniformBuffer`. `vkUpdateDescriptorSets` копирует описание buffer во время вызова, поэтому локальная переменная `bufferInfo` безопасна.

В цикле рисования vertex/index buffers можно привязать один раз, затем для каждого объекта менять descriptor set:

```cpp
VkCommandBuffer cmd = fd.command_buffer;
for (const SceneObject& object : objects) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1,
                            &object.descriptorSet, 0, nullptr);
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
}
```

Это и демонстрирует требуемую идею: геометрия и pipeline общие, параметры экземпляров различаются через наборы дескрипторов.

Альтернативы в реальном проекте — dynamic uniform buffers, push constants, storage buffers и instancing, но формулировка лабораторной прямо просит несколько descriptor sets.

### 17.1 Создание и обновление объектов

Пример сцены из трёх объектов:

```cpp
objects.resize(3);
objects[0].position = {-2.0F, 0.0F, 0.0F};
objects[0].tint = {1.0F, 0.4F, 0.4F};
objects[1].position = {0.0F, 0.0F, 0.0F};
objects[1].tint = {0.4F, 1.0F, 0.4F};
objects[2].position = {2.0F, 0.0F, 0.0F};
objects[2].tint = {0.4F, 0.4F, 1.0F};

for (SceneObject& object : objects) {
    if (!createBuffer(sizeof(UniformData),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      object.uniformBuffer,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
        return false;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(context.allocator,
                         object.uniformBuffer.allocation, &info);
    object.mappedUniformData = info.pMappedData;
    if (object.mappedUniformData == nullptr) {
        return false;
    }
}
```

После этого создаются pool и sets из предыдущего подраздела. В реальной функции при ошибке нужно уничтожить buffers уже обработанных объектов.

Общие `view` и `projection` можно вычислить один раз, а `model` — отдельно:

```cpp
void uploadObjectUniforms(const glm::mat4& view,
                          const glm::mat4& projection) {
    for (SceneObject& object : objects) {
        glm::mat4 model{1.0F};
        model = glm::translate(model, object.position);
        model = glm::rotate(model, glm::radians(object.rotation.x),
                            glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(object.rotation.y),
                            glm::vec3(0, 1, 0));
        model = glm::rotate(model, glm::radians(object.rotation.z),
                            glm::vec3(0, 0, 1));
        model = glm::scale(model, object.scale);

        const UniformData data{
            .model = model,
            .view = view,
            .projection = projection,
            .tint = glm::vec4(object.tint, 1.0F),
        };
        std::memcpy(object.mappedUniformData, &data, sizeof(data));
        vmaFlushAllocation(graphics::internal::context.allocator,
                           object.uniformBuffer.allocation,
                           0, sizeof(data));
    }
}
```

Вызывайте `uploadObjectUniforms` из `render()`, а не из `update()`: к этому моменту fence уже дождался предыдущего кадра. Для анимации изменяйте `position`/`rotation` нужных объектов в `update()`. На паузе перестаньте менять `animationTime`, но продолжайте загружать текущие матрицы.

Очистка:

```cpp
for (SceneObject& object : objects) {
    if (object.uniformBuffer.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator,
                         object.uniformBuffer.handle,
                         object.uniformBuffer.allocation);
    }
}
objects.clear();
```

Сначала дождитесь `vkQueueWaitIdle` и уничтожьте descriptor pool, затем uniform buffers.

## 18. Жизненный цикл приложения

### `application::initialize()`

В рекомендуемом порядке:

1. Сгенерировать `vertices` и `indices`.
2. Создать и заполнить vertex/index buffers.
3. Создать uniform buffer и оставить его mapped.
4. Создать descriptor set layout.
5. Создать descriptor pool и descriptor set.
6. Обновить descriptor set.
7. Прочитать SPIR-V и создать shader modules.
8. Создать pipeline layout.
9. Создать graphics pipeline.

Зависимости определяют порядок: pipeline layout требует готовый descriptor set layout; descriptor set требует layout и pool; descriptor update требует готовый uniform buffer.

Логическая форма функции:

```cpp
bool initialize() {
    MeshData data = createChosenFigure();

    if (!createMeshBuffers(data.vertices, data.indices)) {
        return false;
    }
    if (!createDescriptorResources()) {
        destroyMeshBuffers();
        return false;
    }
    try {
        if (!createGraphicsPipeline()) {
            destroyDescriptorResources();
            destroyMeshBuffers();
            return false;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        destroyDescriptorResources();
        destroyMeshBuffers();
        return false;
    }
    return true;
}
```

Использованные helpers полностью разобраны в разделах 6, 9 и 10. Конкретный `createChosenFigure` выбирает только ваш вариант. Функция выше очищает mesh/descriptor-ресурсы при последующей ошибке; каждый отдельный helper также должен убирать собственные частично созданные handles.

### `application::update(time)`

1. Вычислить `deltaTime`.
2. Построить ImGui.
3. Обновить время анимации.
4. Рассчитать `model`, `view`, `projection`.
5. Сохранить рассчитанный `UniformData` в обычную CPU-переменную.

### `application::render(fd)`

1. Скопировать подготовленные uniform-данные в mapped buffer.
2. Сбросить и начать command buffer.
3. Начать render pass с очисткой color/depth.
4. Установить viewport/scissor.
5. Привязать pipeline.
6. Привязать vertex/index buffers.
7. Для каждого объекта привязать descriptor set и вызвать draw.
8. Завершить render pass и command buffer.

### `application::shutdown()`

После `vkQueueWaitIdle` уничтожать примерно в обратном порядке:

1. Pipeline.
2. Pipeline layout.
3. Descriptor pool.
4. Descriptor set layout.
5. Uniform buffers.
6. Index buffer.
7. Vertex buffer.

Descriptor sets отдельно не уничтожаются, если удаляется их pool.

Пример для одного объекта:

```cpp
void shutdown() {
    auto& context = graphics::internal::context;
    vkQueueWaitIdle(context.graphics_queue);

    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(context.device, pipeline, nullptr);
    }
    if (pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context.device, pipelineLayout, nullptr);
    }
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context.device, descriptorPool, nullptr);
    }
    if (descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context.device,
                                     descriptorSetLayout, nullptr);
    }
    if (uniformBuffer.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator,
                         uniformBuffer.handle,
                         uniformBuffer.allocation);
    }
    if (mesh.indexBuffer.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator,
                         mesh.indexBuffer.handle,
                         mesh.indexBuffer.allocation);
    }
    if (mesh.vertexBuffer.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator,
                         mesh.vertexBuffer.handle,
                         mesh.vertexBuffer.allocation);
    }
}
```

`mappedUniformData` отдельно освобождать не нужно при использовании `VMA_ALLOCATION_CREATE_MAPPED_BIT`: отображение исчезает вместе с allocation.

### 18.1 В каком порядке расположить код в `application.cpp`

C++ должен увидеть объявление имени до места использования. Чтобы не писать дополнительные prototypes, располагайте части так:

```text
1. #include
2. namespace application { namespace {
3. struct Vertex, Buffer, Mesh, MeshData, UniformData, Controls
4. глобальные handles и состояние
5. proceduralColor и генераторы геометрии
6. createBuffer, createAndUploadBuffer, createMeshBuffers
7. readSpirv и createShaderModule
8. create/destroyDescriptorResources
9. createGraphicsPipeline
10. drawControls и makeUniformData
11. } // безымянный namespace
12. initialize, shutdown, update, render
13. } // namespace application
```

Функции интерфейса из `application.hpp` не помещайте в безымянный namespace, иначе `main.cpp` не сможет их вызвать. Helpers и внутренние переменные, наоборот, удобно скрыть там.

Если хотите расположить функцию ниже места вызова, сначала объявите её:

```cpp
bool createGraphicsPipeline(); // declaration

bool initialize() {
    return createGraphicsPipeline();
}

bool createGraphicsPipeline() { // definition
    // ...
}
```

### 18.2 Разница между declaration и definition

Declaration сообщает компилятору имя, аргументы и возвращаемый тип. Definition содержит тело. Одинаковая функция может быть объявлена несколько раз согласованно, но определена в программе должна быть ровно один раз. Ошибка `undefined reference` часто означает, что declaration есть, а подходящей definition нет.

## 19. Особенности текущей заготовки

Текущий проект уже делает следующее:

- создаёт окно GLFW без OpenGL-контекста;
- создаёт Vulkan instance, surface, physical/logical device;
- выбирает graphics queue;
- создаёт swapchain, image views, depth image и framebuffers;
- создаёт основной render pass;
- создаёт command buffer, semaphores и fence;
- интегрирует ImGui;
- пересоздаёт swapchain при изменении окна.

`application::render` уже записывает пустой основной render pass. Объект должен рисоваться внутри него. После этого `graphics::internal::submitAndPresent` записывает отдельный ImGui command buffer и отправляет оба command buffer в очередь.

### Resize

После resize меняется `context.swapchain_extent`. Если viewport/scissor динамические, pipeline пересоздавать не требуется. Матрицу проекции нужно вычислять заново с новым `aspect` каждый кадр или после resize.

### Рабочая директория

Шейдеры загружаются относительным путём, поэтому запускать программу нужно из корня проекта:

```bash
./build-debug/vulkan-starter-app
```

## 20. Синтаксис Vulkan/C++ в проекте

### Designated initializers

Проект использует C++20:

```cpp
VkBufferCreateInfo info{
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = usage,
};
```

Поля должны идти в том же порядке, в котором они объявлены в Vulkan-структуре. Пропущенные поля обнуляются.

### Handles

Объекты Vulkan представлены handles (`VkBuffer`, `VkPipeline`, ...). Нулевое значение:

```cpp
VkPipeline pipeline = VK_NULL_HANDLE;
```

Создание обычно возвращает `VkResult`. Проверяйте его:

```cpp
if (vkCreatePipelineLayout(...) != VK_SUCCESS) {
    std::cerr << "Failed to create pipeline layout\n";
    return false;
}
```

### Размер массива

Вместо ручной константы:

```cpp
.clearValueCount = static_cast<uint32_t>(std::size(clearValues)),
```

Для `std::vector`:

```cpp
uint32_t count = static_cast<uint32_t>(vertices.size());
const Vertex* data = vertices.data();
```

## 21. Частые ошибки

### Окно открывается, но фигуры нет

Проверьте:

- камера не находится внутри объекта;
- объект расположен между `near` и `far`;
- `indexCount` не равен нулю;
- pipeline, buffers и descriptor set действительно привязаны;
- shader paths правильные;
- viewport/scissor имеют ненулевой размер;
- порядок граней соответствует `frontFace`;
- матрицы перемножаются как `projection * view * model`.

### Фигура вверх ногами

Для Vulkan после `glm::perspective`/`glm::ortho` выполните:

```cpp
projection[1][1] *= -1.0F;
```

### Фигура искажена после изменения окна

Пересчитайте `aspect = width / height`. Оба операнда должны быть `float`, иначе получится целочисленное деление.

### Мерцание или случайные данные

Возможные причины:

- CPU меняет buffer, пока GPU его читает;
- C++ layout uniform не совпадает с GLSL `std140`;
- ресурс уничтожен до завершения очереди;
- command buffer записывается без reset или пока ещё выполняется.

### Validation error про image layout или unrecorded command buffer

Command buffer должен пройти состояния reset → begin → команды → end до `vkQueueSubmit`. Render pass обеспечивает переход swapchain image из `UNDEFINED` в `COLOR_ATTACHMENT_OPTIMAL`, а ImGui pass — в `PRESENT_SRC_KHR`.

### `Failed to initialize GLFW`

Обычно процесс не имеет доступа к графической сессии (`DISPLAY`/Wayland), например запускается из изолированного терминала. Это не ошибка компиляции Vulkan.

### CMake не компилирует шейдеры

Установите Vulkan SDK или пакет, содержащий `glslc`, затем заново выполните конфигурацию:

```bash
cmake --preset debug
cmake --build build-debug --parallel
```

## 22. Инструменты диагностики

Всегда оставляйте validation layers включёнными в debug-сборке. Их сообщения обычно содержат:

- имя нарушенного правила (`VUID-...`);
- тип и handle объекта;
- ожидаемое и фактическое состояние.

Полезные проверки:

```bash
cmake --preset debug
cmake --build build-debug --parallel
./build-debug/vulkan-starter-app
```

Это три разных этапа:

1. `cmake --preset debug` — **конфигурация**: CMake находит Vulkan, загружает зависимости и генерирует Ninja-файлы в `build-debug`.
2. `cmake --build ...` — **сборка**: компилируются C++, GLSL и создаётся executable.
3. `./build-debug/...` — **запуск** уже собранной программы.

После изменения только `.cpp` обычно достаточно повторить этап 2. После изменения `CMakeLists.txt` повторите этапы 1 и 2. Shader является build dependency, поэтому изменение `.vert`/`.frag` должно вызвать повторный `glslc` на этапе 2.

Не запускайте так:

```bash
cd build-debug
./vulkan-starter-app
```

В этом случае относительный путь `shaders/...` будет искаться внутри `build-debug`. Оставайтесь в корне проекта.

### 22.1 Как читать вывод сборки

- Строка с `error:` останавливает сборку; исправлять нужно первую содержательную ошибку, остальные могут быть её следствием.
- `warning:` не останавливает сборку, но предупреждения вашего кода стоит исправлять.
- Предупреждения внутри `build-debug/_deps/...` относятся к сторонним библиотекам.
- `undefined reference` — объявление функции найдено, но её реализация не скомпонована или сигнатура различается.
- `No such file or directory` для header — неверный include или зависимость не подключена к target.
- Ошибка открытия `.spv` во время запуска — shader не скомпилирован или рабочая директория неверна.

### 22.2 Как читать validation error

Сначала найдите имя функции, например `vkQueueSubmit` или `vkCmdDrawIndexed`. Затем прочитайте фразу `expects ... instead ...`: она обычно прямо называет фактическое и требуемое состояние. Идентификатор `VUID-...` позволяет найти правило в спецификации, но для лабораторной чаще достаточно текста сообщения.

Исправляйте первую validation error и запускайте снова: одна ранняя ошибка часто вызывает цепочку последующих.

Ошибки CMake — это этап конфигурации. Ошибки компилятора — C++/типы/заголовки. Ошибки `glslc` — GLSL. Сообщения validation layers — некорректное использование Vulkan во время выполнения.

## 23. План реализации по маленьким проверяемым шагам

1. **Выбрать фигуру.** На бумаге определить вершины и грани.
2. **Подключить GLM.** Убедиться, что пустой проект собирается.
3. **Создать GLSL-файлы.** Включить `compile_shader` и проверить `.spv`.
4. **Описать `Vertex`.** Сгенерировать массивы и вывести их размеры в консоль.
5. **Создать vertex/index buffers.** Проверить коды возврата VMA.
6. **Создать uniform buffer и descriptor set.** Сначала передавать единичные матрицы.
7. **Создать pipeline.** На первом запуске отключить culling.
8. **Добавить draw commands.** Добиться статичной фигуры без validation errors.
9. **Добавить view и perspective.** Проверить depth test вращением объекта.
10. **Добавить ImGui-трансформации.** Position, rotation, scale.
11. **Добавить выбор проекции.** Проверить resize окна.
12. **Добавить tint и процедурные цвета.** Проверить умножение в fragment/vertex shader.
13. **Добавить time-based анимацию.** Пауза не должна менять фазу.
14. **Добавить несколько объектов.** Отдельный UBO и descriptor set для каждого.
15. **Проверить cleanup.** Закрытие окна не должно выдавать validation errors.

## 24. Чек-лист готовой работы

- [ ] Выбранная фигура геометрически корректна.
- [ ] Используются vertex и index buffers.
- [ ] Работают depth test и back-face culling.
- [ ] Перспективная проекция выглядит естественно.
- [ ] Ортографическая проекция переключается без пересоздания pipeline.
- [ ] Position, rotation и scale управляются из ImGui.
- [ ] Масштаб ограничен ненулевыми значениями.
- [ ] Анимация зависит от времени, имеет паузу и скорость.
- [ ] Траектория имеет настраиваемый параметр, например радиус.
- [ ] Цвет меняется через `ColorEdit`.
- [ ] Вершины имеют процедурно рассчитанные цвета.
- [ ] Выбранный tint умножается на цвет вершины.
- [ ] Для оценки 5 рисуется несколько объектов с несколькими descriptor sets.
- [ ] Resize окна не ломает пропорции и отрисовку.
- [ ] Нет ошибок validation layers при работе и закрытии.
- [ ] Программа собирается командами из README и запускается из корня проекта.

## 25. Что нужно уметь объяснить на защите

1. Зачем нужны model, view и projection matrices и в каком порядке они применяются.
2. Чем перспективная проекция отличается от ортографической.
3. Зачем нужны vertex/index/uniform buffers.
4. Что связывает `Vertex`, vertex shader locations и vertex input pipeline state.
5. Что хранит descriptor set и зачем нужен descriptor set layout.
6. Почему необходим depth buffer.
7. Что такое command buffer, render pass, framebuffer и pipeline.
8. Как работает процедурная окраска вершин.
9. Почему анимация использует `deltaTime`, а не приращение за кадр.
10. Как одна mesh рисуется несколько раз с разными descriptor sets.
11. Почему ресурсы уничтожаются только после завершения работы GPU.

Если эти вопросы понятны и каждый пункт чек-листа выполнен, лабораторная закрывает все требования задания.

## 26. Краткий словарь терминов

- **Attachment** — image, используемое render pass как цвет или глубина.
- **Binding** — пронумерованное место для vertex buffer или descriptor resource.
- **Culling** — отбрасывание граней, обычно обращённых от камеры.
- **Descriptor** — описание GPU-ресурса, доступного shader.
- **Draw call** — команда рисования, например `vkCmdDrawIndexed`.
- **Extent** — ширина и высота изображения.
- **Fragment** — кандидат на итоговый пиксель после растеризации.
- **Framebuffer** — набор конкретных attachments для render pass.
- **Handle** — идентификатор Vulkan-объекта.
- **Index** — номер вершины в vertex buffer.
- **Layout изображения** — режим использования image, например color attachment или present. Это не то же самое, что pipeline layout.
- **Mesh** — геометрическая сетка: вершины и индексы.
- **NDC** — нормализованное пространство после perspective divide.
- **Pipeline** — настроенный графический конвейер.
- **Rasterization** — превращение треугольников в fragments.
- **Shader** — небольшая программа, выполняемая GPU.
- **SPIR-V** — бинарное представление shader для Vulkan.
- **Staging buffer** — временный CPU-visible buffer для копирования в быструю GPU-память.
- **Swapchain** — изображения, циклически рисуемые и показываемые в окне.
- **UBO / uniform buffer** — buffer небольших общих shader-данных.
- **Validation layer** — диагностический слой, проверяющий корректность Vulkan-вызовов.
- **Vertex** — вершина и её атрибуты: позиция, цвет и другие данные.
- **Viewport** — преобразование NDC в область framebuffer.
- **VMA** — библиотека, упрощающая выделение Vulkan-памяти.

## 27. Границы материала

Этот файл покрывает знания, необходимые именно для всех пунктов лабораторной №1: построение сетки, матрицы, две проекции, трансформации, UI, анимацию, цвета, Vulkan pipeline, descriptors и несколько объектов.

Он намеренно не превращается в полный учебник по всему Vulkan. За пределами задания остаются:

- физически корректное освещение и normal matrices;
- текстуры, samplers и mipmaps;
- загрузка моделей из файлов;
- compute shaders и ray tracing;
- несколько кадров одновременно в обработке;
- оптимизированные staging uploads;
- полноценная система камеры;
- RAII-обёртки и промышленная обработка всех сбоев;
- dynamic rendering и современные альтернативы классическому render pass.

Эти темы не нужны для выполнения или защиты текущего задания. Добавлять их до завершения обязательных пунктов вредно: они увеличат код и число возможных ошибок, не улучшая оценку за лабораторную.

### 27.1 Критерий «я действительно разобрался»

Материал усвоен не тогда, когда код скопирован, а когда вы можете без подсказки:

1. Нарисовать на бумаге путь одной вершины из model space до экрана.
2. Объяснить, откуда берутся `vertices` и `indices` вашей фигуры.
3. Найти соответствия между `Vertex`, pipeline attributes и GLSL inputs.
4. Объяснить, почему uniform buffer нельзя менять, пока GPU его читает.
5. Добавить второй объект, не копируя pipeline и mesh.
6. По validation error определить этап, на котором нарушено состояние.
7. Изменить один параметр UI и проследить его путь до итогового пикселя.

Если любой пункт пока не получается, вернитесь к соответствующему разделу и сделайте минимальный эксперимент: один треугольник, единичные матрицы, один descriptor set или отключённый culling. Такой способ быстрее полного переписывания программы.
