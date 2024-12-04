#include <iostream>
#include <functional>
#include <thread>
#include <future>
#include <chrono>

#include "threadpool.h"

using namespace std;

/*
* 如何能让线程池提交任务更加方便
* 1.pool.submitTask(sum1, 10, 20);
*   pool.submitTask(sum2, 1, 2, 3);
*   submitTask:可变参模板编程
* 
* 2.我们自己造了一个Result以及相关的类型,代码太多
*   C++11 线程库 thread package_task(function函数对象) async
*   使用future来代替Result节省线程池代码
*/

int sum1(int a, int b) {
	this_thread::sleep_for(chrono::seconds(5));
	return a + b;
}

int sum2(int a, int b, int c) {
	this_thread::sleep_for(chrono::seconds(5));
	return a + b + c;
}

int main()
{
	ThreadPool pool;
	pool.start(2);

	future<int> r1 = pool.submitTask(sum2, 1, 2, 3);
	future<int> r2 = pool.submitTask(sum1, 10, 20);
	future<int> r3 = pool.submitTask([](int a, int b)->int {
		int sum = 0;
		for (int i = a; i <= b; i++) sum += i;
		return sum;
		}, 1, 100);
	future<int> r4 = pool.submitTask(sum1, 1, 2);
	pool.submitTask(sum1, 10, 20);
	pool.submitTask(sum1, 10, 20);
	cout << r1.get() << endl;
	cout << r2.get() << endl;
	cout << r3.get() << endl;
	cout << r4.get() << endl;

	return 0;

	/*
	packaged_task<int(int, int)> task(sum1);
	// future <==> Result
	task(10, 20);
	future<int> res = task.get_future();
	//task(10, 20);
	cout << res.get() << endl;
	return 0;
	*/
}

