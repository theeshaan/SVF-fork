#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <memory>

using namespace std;

/* =========================
   Utility Functions
   ========================= */

int add(int a, int b) {
    return a + b;
}

int multiply(int a, int b) {
    return a * b;
}

void swapValues(int* a, int* b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

void printLine() {
    cout << "-----------------------------" << endl;
}

/* =========================
   Struct Example
   ========================= */

struct Point {
    int x;
    int y;

    void print() const {
        cout << "(" << x << ", " << y << ")" << endl;
    }
};

/* =========================
   Class Example
   ========================= */

class Person {
private:
    string name;
    int age;
    int* id;  // pointer member

public:
    Person(const string& name, int age, int idValue)
        : name(name), age(age) {
        id = new int(idValue);
    }

    // Copy constructor
    Person(const Person& other) {
        name = other.name;
        age = other.age;
        id = new int(*other.id);
    }

    // Destructor
    ~Person() {
        delete id;
    }

    void birthday() {
        age++;
    }

    int getAge() const {
        return age;
    }

    int getId() const {
        return *id;
    }

    void print() const {
        cout << "Person{name=" << name
             << ", age=" << age
             << ", id=" << *id << "}" << endl;
    }
};

/* =========================
   Vector Operations
   ========================= */

void fillVector(vector<int>& v, int n) {
    for (int i = 0; i < n; ++i) {
        v.push_back(i * 2);
    }
}

void printVector(const vector<int>& v) {
    for (int value : v) {
        cout << value << " ";
    }
    cout << endl;
}

int sumVector(const vector<int>& v) {
    return accumulate(v.begin(), v.end(), 0);
}

void sortDescending(vector<int>& v) {
    sort(v.begin(), v.end(), greater<int>());
}

/* =========================
   Pointer & Dynamic Memory
   ========================= */

int* createDynamicArray(int size) {
    int* arr = new int[size];
    for (int i = 0; i < size; ++i) {
        arr[i] = i + 1;
    }
    return arr;
}

void printArray(const int* arr, int size) {
    for (int i = 0; i < size; ++i) {
        cout << arr[i] << " ";
    }
    cout << endl;
}

void deleteArray(int* arr) {
    delete[] arr;
}

/* =========================
   Smart Pointer Example
   ========================= */

unique_ptr<int> createSmartInt(int value) {
    return make_unique<int>(value);
}

/* =========================
   Algorithm Examples
   ========================= */

void removeEvenNumbers(vector<int>& v) {
    v.erase(remove_if(v.begin(), v.end(),
                      [](int x) { return x % 2 == 0; }),
            v.end());
}

bool containsValue(const vector<int>& v, int value) {
    return find(v.begin(), v.end(), value) != v.end();
}

/* =========================
   Main Function
   ========================= */

int main() {
    printLine();
    cout << "Basic math functions" << endl;
    cout << "Add: " << add(3, 4) << endl;
    cout << "Multiply: " << multiply(5, 6) << endl;

    printLine();
    cout << "Pointer swapping" << endl;
    int a = 10, b = 20;
    swapValues(&a, &b);
    cout << "a = " << a << ", b = " << b << endl;

    printLine();
    cout << "Struct example" << endl;
    Point p{3, 7};
    p.print();

    printLine();
    cout << "Class and dynamic memory" << endl;
    Person person1("Alice", 30, 101);
    Person person2 = person1;
    person2.birthday();
    person1.print();
    person2.print();

    printLine();
    cout << "Vector operations" << endl;
    vector<int> numbers;
    fillVector(numbers, 10);
    printVector(numbers);

    cout << "Sum: " << sumVector(numbers) << endl;
    sortDescending(numbers);
    printVector(numbers);

    removeEvenNumbers(numbers);
    printVector(numbers);

    cout << "Contains 7? " << (containsValue(numbers, 7) ? "Yes" : "No") << endl;

    printLine();
    cout << "Dynamic array" << endl;
    int size = 5;
    int* arr = createDynamicArray(size);
    printArray(arr, size);
    deleteArray(arr);

    printLine();
    cout << "Smart pointer" << endl;
    auto smartInt = createSmartInt(42);
    cout << "Smart value: " << *smartInt << endl;

    printLine();
    cout << "Program finished successfully" << endl;

    return 0;
}
