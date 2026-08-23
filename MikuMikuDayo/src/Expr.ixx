/*

式→スタックマシン変換器
MMXXV (c) SANDMAN presents!!

変数・関数・演算子を含む文字列を入力するとそれを高速に評価するための仮想スタックマシンを作る


入力文字列の文字コードはUTF-8
内部的な計算はfloat型に変換して行われ、返り値はfloat型で返る

つかいかた

	1.Compilerインスタンスを作る
	Compiler compiler;

	2.式インスタンスを作り、コンパイルする
	Expr expr;
	expr.Compile(compiler, "100+200");

	3.式を評価する
	float result = expr.Eval();		//resiltに300が入る


変数がある場合の使い方

	float varA = 1.0f;
	compiler.fvars["a"] = &varA;	//式中の変数aは、ホストアプリケーションのvarAに紐づけられる。varAを便宜的に「ホスト変数」と呼ぶ
	expr.Compile(compiler, "a*100");
	float result = expr.Eval();		//resultに100が入る
	varA = 2.0f;
	result = expr.Eval();			//resultに200が入る
	
expr.Eval()時の注意点
・変数や定数はExpr::Compile()実行時点でのcompilerの状態を元に解決されるため、compilerは解放されていてもよい
・式中で参照されている変数のホスト変数が解放されていてはならない

*/


module;

#pragma once
#include <functional>
#include <regex>
#include <stack>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

export module Expr;

export namespace Expr {

	//関数
	struct Function {
		std::function<float(const std::vector<float>&)> callback;	//処理コード
		int nArg;	//関数が要求する引数の数
	};

	//演算子
	enum class Operator { opERR, opEQ, opNEQ, opGT, opGEQ, opLT, opLEQ, opOR, opAND, opNOT, opADD, opSUB, opMUL, opDIV, opUPLUS, opUMINUS };

	//式の要素(割り当てなし・演算子・定数・float変数・int変数・関数 のうちどれか)
	struct Op {
		std::variant<std::monostate, Operator, float, float*, int*, Function> data;
		Op(Operator o) : data(o) {}
		Op(float lit) : data(lit) {}
		Op(float* fvar) : data(fvar) {}
		Op(int* ivar) : data(ivar) {}
		Op(const Function& f) : data(f) {}
	};

	class Expr;

	//式コンパイラ
	class Compiler {
	private:
		std::unordered_map<std::string, Operator>operators;	//サポートする演算子についての情報(拡張不可)
		std::vector<std::string> Tokenize(const std::string& expression);
		std::vector<std::string> ToRPN(const std::vector<std::string>& tokens);
	public:
		std::string error;	//コンパイル失敗時のエラーメッセージ
		std::unordered_map<std::string, Function>functions;	//サポートする関数群についての情報
		std::unordered_map<std::string, float>constants;	//サポートする定数についての情報
		std::unordered_map<std::string, float*>fvars;		//サポートする変数名についての情報
		std::unordered_map<std::string, int*>ivars;			//サポートするint型の変数名についての情報
		Compiler();
		friend Expr;
	};

	//式
	class Expr {
	private:
		std::vector<Op> m_ops;
		bool m_const = true;	//Compileされてない式の場合は定数0扱いされる(例外などは投げない)
		float m_constVal = 0.0f;
		Compiler* m_compiler;
	public:
		//inputをコンパイルする。成功すればtrue
		//Exprインスタンスのライフタイムの間、compilerは生きている事が前提なので注意
		bool Compile(Compiler& compiler, const std::string& input);
		//定数式?
		bool IsConst() const { return m_const; }
		//定数式だった場合、その値
		float ConstValue() const { return m_constVal; }
		//評価した結果を返す cachedがtrueかつ定数式の場合は評価をスキップして定数を返す
		float Eval(bool cached = true) const;

		friend Compiler;
	};

}


module : private;

namespace Expr {

	float maxf(float a, float b) { return a > b ? a : b; };
	float minf(float a, float b) { return a < b ? a : b; };

	struct uint3 {
		std::uint32_t x, y, z;
	};

	Compiler::Compiler()
	{
		auto sat = [](float f) { return maxf(0.0f, minf(1.0f, f)); };
		auto frac = [](float f) { return f - floorf(f); };
		auto lerp = [](float a, float b, float c) { return a * (1.0f - c) + b * c; };
		auto hsvR = [&](float h, float s, float v) { return (((sat(abs(frac(h) * 2.0f - 1.0f) * 3.0f - 1.0f) - 1.0f) * sat(s) + 1.0f) * v); };
		auto hsvG = [&](float h, float s, float v) { return hsvR(h + 2.0f / 3.0f, s, v); };
		auto hsvB = [&](float h, float s, float v) { return hsvR(h + 1.0f / 3.0f, s, v); };
		auto uvec3 = [&](const uint3& v, auto f) { return uint3(f(v.x), f(v.y), f(v.z)); };
		//pcg3d
		auto hash = [&](float x) {
			auto v = uint3((uint32_t)x, 114, 514);
			v = uvec3(v, [&](auto x) {return x * 1664525u + 1013904223u; });
			v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
			v = uvec3(v, [&](auto x) { return x ^ (x >> 16u); });
			v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
			return (float)((double)v.x / 4294967296);
			};
		auto noise = [&](float x) {
			float f = frac(x);
			float i = x - f;
			float u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
			float n = lerp(hash(i), hash(i + 1), u);
			return n;
			};

		const float PI = acosf(-1.0f);
		functions.emplace("sin", Function{ [](const std::vector<float>& a)->float {return sinf(a[0]); }, 1});
		functions.emplace("cos", Function{ [](const std::vector<float>& a)->float {return cosf(a[0]); }, 1});
		functions.emplace("tan", Function{ [](const std::vector<float>& a)->float {return tanf(a[0]); }, 1 });
		functions.emplace("asin", Function{ [](const std::vector<float>& a)->float {return asinf(a[0]); }, 1 });
		functions.emplace("acos", Function{ [](const std::vector<float>& a)->float {return acosf(a[0]); }, 1 });
		functions.emplace("atan", Function{ [](const std::vector<float>& a)->float {return atanf(a[0]); }, 1 });
		functions.emplace("atan2", Function{ [](const std::vector<float>& a)->float {return atan2f(a[0],a[1]); }, 2});
		functions.emplace("sinh", Function{ [](const std::vector<float>& a)->float {return sinhf(a[0]); }, 1 });
		functions.emplace("cosh", Function{ [](const std::vector<float>& a)->float {return coshf(a[0]); }, 1 });
		functions.emplace("tanh", Function{ [](const std::vector<float>& a)->float {return tanhf(a[0]); }, 1 });

		functions.emplace("exp", Function{ [](const std::vector<float>& a)->float {return expf(a[0]); }, 1 });
		functions.emplace("log", Function{ [](const std::vector<float>& a)->float {return logf(a[0]); }, 1 });
		functions.emplace("sqrt", Function{ [](const std::vector<float>& a)->float {return sqrtf(a[0]); }, 1 });
		functions.emplace("exp2", Function{ [](const std::vector<float>& a)->float {return exp2f(a[0]); }, 1 });
		functions.emplace("log2", Function{ [](const std::vector<float>& a)->float {return log2f(a[0]); }, 1 });
		functions.emplace("log10", Function{ [](const std::vector<float>& a)->float {return log10f(a[0]); }, 1 });

		functions.emplace("abs", Function{ [](const std::vector<float>& a)->float { return fabsf(a[0]); }, 1 });
		functions.emplace("floor", Function{ [](const std::vector<float>& a)->float {return floorf(a[0]); }, 1 });
		functions.emplace("ceil", Function{ [](const std::vector<float>& a)->float {return ceilf(a[0]); }, 1 });
		functions.emplace("trunc", Function{ [](const std::vector<float>& a)->float {return truncf(a[0]); }, 1 });
		functions.emplace("round", Function{ [](const std::vector<float>& a)->float {return roundf(a[0]); }, 1 });
		functions.emplace("frac", Function{ [frac](const std::vector<float>& a)->float {return frac(a[0]); }, 1 });
		functions.emplace("fmod", Function{ [](const std::vector<float>& a)->float {return fmodf(a[0],a[1]); }, 2 });
		functions.emplace("mod", Function{ [](const std::vector<float>& a)->float {return a[0] - a[1] * floorf(a[0]/a[1]); }, 2});
		functions.emplace("sign", Function{ [](const std::vector<float>& a)->float { if (a[0] == 0.0f) return 0.0f; else if (a[0] > 0.0f) return 1.0f; else return -1.0f; }, 1 });

		functions.emplace("degrees", Function{ [PI](const std::vector<float>& a)->float {return a[0] * 180.0f / PI; }, 1 });
		functions.emplace("radians", Function{ [PI](const std::vector<float>& a)->float {return a[0] * PI / 180.0f; }, 1 });

		functions.emplace("min", Function{ [](const std::vector<float>& a)->float { return minf(a[0],a[1]); }, 2 });
		functions.emplace("max", Function{ [](const std::vector<float>& a)->float { return maxf(a[0],a[1]); }, 2 });
		functions.emplace("clamp", Function{ [](const std::vector<float>& a)->float { return maxf(minf(a[2], a[0]), a[1]); }, 3 });
		functions.emplace("saturate", Function{ [sat](const std::vector<float>& a)->float {return sat(a[0]); }, 1 });
		
		functions.emplace("lerp", Function{ [lerp](const std::vector<float>& a)->float { return lerp(a[0],a[1],a[2]); }, 3 });
		functions.emplace("step", Function{ [](const std::vector<float>& a)->float { return a[0] >= a[1]; }, 2 });
		functions.emplace("smoothstep", Function{ [](const std::vector<float>& a)->float {
			float v1 = a[0];
			float v2 = a[1];
			float v3 = a[2];
			if (v1 == v2) { return v1 <= v3 ? 0.0f : 1.0f; }
			float u = (v3 - v1) / (v2 - v1);
			float x = maxf(0.0f, minf(u, 1.0f));
			return x * x * (3.0f - 2.0f * x);
		}, 3 });


		functions.emplace("bit", Function{ [](const std::vector<float>& a)->float { return (float)(((int)a[0] >> (int)a[1]) & 1); }, 2});

		functions.emplace("hsvR", Function{ [&](const std::vector<float>& a)->float {return hsvR(a[0],a[1],a[2]); }, 3 });
		functions.emplace("hsvG", Function{ [&](const std::vector<float>& a)->float {return hsvG(a[0],a[1],a[2]); }, 3 });
		functions.emplace("hsvB", Function{ [&](const std::vector<float>& a)->float {return hsvB(a[0],a[1],a[2]); }, 3 });

		functions.emplace("select", Function{ [](const std::vector<float>& a)->float {return a[0] ? a[1] : a[2]; }, 3 });

		functions.emplace("hash", Function{ [&](const std::vector<float>& a)->float {return hash(a[0]); }, 1 });
		functions.emplace("noise", Function{ [&](const std::vector<float>& a)->float {return noise(a[0]); }, 1 });


		constants.emplace("pi", PI);

		operators.emplace("==", Operator::opEQ);
		operators.emplace("!=", Operator::opNEQ);
		operators.emplace(">", Operator::opGT);
		operators.emplace(">=", Operator::opGEQ);
		operators.emplace("<", Operator::opLT);
		operators.emplace("<=", Operator::opLEQ);
		operators.emplace("||", Operator::opOR);
		operators.emplace("&&", Operator::opAND);
		operators.emplace("!", Operator::opNOT);
		operators.emplace("+", Operator::opADD);
		operators.emplace("-", Operator::opSUB);
		operators.emplace("*", Operator::opMUL);
		operators.emplace("/", Operator::opDIV);
		operators.emplace("u+", Operator::opUPLUS);
		operators.emplace("u-", Operator::opUMINUS);
	}


	//数値に使える文字かどうかを判定(1e+3 みたいなのはダメ)
	bool isNumber(char c)
	{
		return ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-');
	}

	//シンボルに使える文字かどうかを判定
	bool isSymbol(char c)
	{
		return std::isalnum(unsigned char(c)) || c == '_';
	}

	//文字列をトークン列に変換
	std::vector<std::string> Compiler::Tokenize(const std::string& src) {
		static const std::regex re(R"((<=|>=|==|!=|\|\||&&)|(\d*\.\d+|\d+(\.\d+)?)|([A-Za-z_]\w*(?:\[\d+\])*)|[+\-*/<>=!(),])", std::regex::ECMAScript);

		std::vector<std::string> tokens;
		for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
			it != std::sregex_iterator(); ++it) {
			auto tok = it->str();
			//定数を展開
			if (constants.contains(tok))
				tok = std::to_string(constants[tok]);
			tokens.push_back(tok);
		}

		// ——ここで単項＋/− の後処理——
		// トークン列の先頭 or 直前が "(" か演算子だったら単項扱いにする
		auto isOp = [&](const std::string& t) {
			return t == "+" || t == "-" || t == "*" || t == "/" ||
				t == "==" || t == "!=" || t == "<=" || t == ">=" ||
				t == "<" || t == ">" || t == "||" || t == "&&" ||
				t == "!" || t == "u+" || t == "u-";
			};
		for (size_t i = 0; i < tokens.size(); ++i) {
			if ((tokens[i] == "+" || tokens[i] == "-") &&
				(i == 0 || tokens[i - 1] == "(" || isOp(tokens[i - 1]))) {
				tokens[i] = std::string("u") + tokens[i];
			}
		}

		return tokens;
	}



	// tokenizeで切り出してトークン列にした物を逆ポーランド記法に従って並べ替える
	std::vector<std::string> Compiler::ToRPN(const std::vector<std::string>& tokens) {
		error = "";
		std::vector<std::string> output;
		std::stack<std::string> operators;
		std::unordered_map<std::string, int> precedence = {
			{ "||", 0},
			{ "&&", 1},
			{ "==", 3},{ "!=", 3},
			{ "<",4},{ "<=", 4},{ ">", 4},{ ">=", 4},
			{ "+", 5 }, { "-", 5},
			{ "*", 6}, { "/", 6},
			{ "!", 7},
			{"u+", 8}, {"u-",8}
		};
		std::unordered_map<std::string, int> associativity = {
			{"||", 0}, {"&&", 0}, {"==", 0}, {"!=", 0},
			{"<", 0}, {"<=", 0}, {">", 0}, {">=", 0},
			{"+", 0}, {"-", 0}, {"*", 0}, {"/", 0},
			{"!", 1}, {"u+", 1}, {"u-", 1}
		}; // 0 for left, 1 for right

		for (size_t i = 0; i < tokens.size(); ++i) {
			const std::string& token = tokens[i];

			// 数値または変数の場合
			if (std::regex_match(token, std::regex(R"(\-?\d*\.\d+|\-?\d+|[a-zA-Z_]\w*(?:\[\d+\])*)")) && functions.find(token) == functions.end()) {
				output.push_back(token);
			}
			// 左括弧の場合
			else if (token == "(") {
				operators.push(token);
			}
			// 右括弧の場合
			else if (token == ")") {
				while (!operators.empty() && operators.top() != "(") {
					output.push_back(operators.top());
					operators.pop();
				}
				if (!operators.empty() && operators.top() == "(") {
					operators.pop();
				} else {
					throw std::runtime_error("Mismatched parentheses");
				}
				// 関数を処理する
				if (!operators.empty() && functions.find(operators.top().substr(1)) != functions.end()) {
					output.push_back(operators.top());
					operators.pop();
				}
			}
			// 関数の場合
			else if (functions.find(token) != functions.end()) {
				operators.push("@" + token); // 関数トークンを追加
			}
			// カンマの場合（関数の引数区切り）
			else if (token == ",") {
				while (!operators.empty() && operators.top() != "(") {
					output.push_back(operators.top());
					operators.pop();
				}
			}
			// 演算子の場合
			else if (precedence.find(token) != precedence.end()) {
				while (!operators.empty() && operators.top() != "(" &&
					((associativity[token] == 0 && precedence[token] <= precedence[operators.top()]) ||
						(associativity[token] == 1 && precedence[token] < precedence[operators.top()]))) {
					output.push_back(operators.top());
					operators.pop();
				}
				operators.push(token);
			}
		}

		while (!operators.empty()) {
			if (operators.top() == "(" || operators.top() == ")") {
				error =  "Mismatched parentheses";
				return {};
			}
			output.push_back(operators.top());
			operators.pop();
		}

		return output;
	}

	bool is_float_regex(const std::string& s) {
		static const std::regex re(
			R"(^[+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?$)"
		);
		return std::regex_match(s, re);
	}

	bool Expr::Compile(Compiler& compiler, const std::string& input)
	{
		m_compiler = &compiler;
		m_compiler->error = "";

		auto tokens = compiler.Tokenize(input);
		auto postfix = compiler.ToRPN(tokens);
		if (!compiler.error.empty()) {
			return false;
		}

		//後置記法になったところでm_opsを構成する
		m_ops = {};
		for (auto&& p : postfix) {
			if (p.empty())
				continue;
			if (compiler.operators.contains(p)) {
				m_ops.emplace_back(compiler.operators[p]);
			} else if (compiler.fvars.contains(p)) {
				m_ops.emplace_back(compiler.fvars[p]);
			} else if (compiler.ivars.contains(p)) {
				m_ops.emplace_back(compiler.ivars[p]);
			} else if (p.size() >= 2 && p[0] == '@' && compiler.functions.contains(p.substr(1))) {
				m_ops.emplace_back(compiler.functions[p.substr(1)]);
			} else if (is_float_regex(p)) {
				char* dmy;
				float f = std::strtof(p.c_str(), &dmy);
				m_ops.emplace_back(f);
			} else {
				compiler.error = "Unknown variable or function \"" + p + "\"";
				return false;
			}
		}

		//定数式の場合はその値
		m_constVal =  Eval(false);

		//定数式？
		m_const = true;
		for (auto&& op : m_ops) {
			if (std::holds_alternative<int*>(op.data) || std::holds_alternative<float*>(op.data)) {
				m_const = false;
				break;
			}
		}

		//エラーが出てないか？
		return compiler.error.empty();
	}



	//定数式の後置記法トークン列を評価する
	float Expr::Eval(bool cached) const {
		if (cached && m_const) {
			return m_constVal;
		}

		std::stack<float> st;
		m_compiler->error.clear();

		// visitor を用意（C++17 以降）
		auto visitor = [&](auto&& v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, float>) {
				// リテラル
				st.push(v);
			} else if constexpr (std::is_same_v<T, float*>) {
				// 変数(float型)
				st.push(*v);
			} else if constexpr (std::is_same_v<T, int*>) {
				// 変数(int型)
				st.push((float)(*v));
			} else if constexpr (std::is_same_v<T, Function>) {
				// 関数呼び出し
				if (st.size() < static_cast<size_t>(v.nArg)) {
					m_compiler->error = "Too few arguments for function";
					return;
				}
				std::vector<float> args;
				args.reserve(v.nArg);
				size_t n = v.nArg;
				while (n--) {
					args.push_back(st.top());
					st.pop();
				}
				// 引数をもとに戻す（必要に応じ）
				std::reverse(args.begin(), args.end());
				st.push(v.callback(args));
			} else if constexpr (std::is_same_v<T, Operator>) {
				// 演算子
				switch (v) {
				case Operator::opNOT: {
					if (st.empty()) { m_compiler->error = "Missing operand for !"; return; }
					float a = st.top(); st.pop();
					st.push(a == 0.0f ? 1.0f : 0.0f);
					break;
				}
				case Operator::opUMINUS:
					if (st.empty()) { m_compiler->error = "Missing operand for unary -"; return; }
					{ float a = st.top(); st.pop(); st.push(-a); }
					break;
				case Operator::opUPLUS: break;	//なにもしなくてよい
				case Operator::opADD:
				case Operator::opSUB:
				case Operator::opMUL:
				case Operator::opDIV:
				case Operator::opEQ:
				case Operator::opNEQ:
				case Operator::opGT:
				case Operator::opGEQ:
				case Operator::opLT:
				case Operator::opLEQ:
				case Operator::opAND:
				case Operator::opOR: {
					if (st.size() < 2) { m_compiler->error = "Missing operands"; return; }
					float b = st.top(); st.pop();
					float a = st.top(); st.pop();
					float res = 0;
					switch (v) {
					case Operator::opADD: res = a + b; break;
					case Operator::opSUB: res = a - b; break;
					case Operator::opMUL: res = a * b; break;
					case Operator::opDIV: res = a / b; break;
					case Operator::opEQ:  res = (a == b); break;
					case Operator::opNEQ: res = (a != b); break;
					case Operator::opGT:  res = (a > b);  break;
					case Operator::opGEQ: res = (a >= b); break;
					case Operator::opLT:  res = (a < b);  break;
					case Operator::opLEQ: res = (a <= b); break;
					case Operator::opAND: res = (a != 0.0f && b != 0.0f); break;
					case Operator::opOR:  res = (a != 0.0f || b != 0.0f); break;
					default: break;
					}
					st.push(res);
					break;
				}
				default:
					m_compiler->error = "Unknown operator";
					return;
				}
			} else {
				m_compiler->error = "Invalid Op variant";
				return;
			}
			};

		// 実行ループ
		for (auto& op : m_ops) {
			std::visit(visitor, op.data);
			if (!m_compiler->error.empty())
				return 0.0f;  // エラー早期脱出
		}

		if (st.size() != 1) {
			m_compiler->error = "Expression did not reduce to single value";
			return 0.0f;
		}
		return st.top();
	}
}
